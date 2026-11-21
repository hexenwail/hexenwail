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
    * R_BuildWorldCull returns under R_GPU () before its programs (compute is
      P5); alias instancing and GL_DrawAliasSkeletal need
      gl_renderer_caps.shader_storage, which GL_Init_NullGL leaves false.
      (Through P2c R_BuildWorldVBO and R_BuildSkyStencilVBO returned too, and
      the world took the GL_ImmEnd fallbacks; P2d below turns that round.)
  stencil under -gpu (#340 P2c), which tools/sweep-skybox-gpu.steps checks
  frame by frame:
    * GL_Init_NullGL sets have_stencil = RG_DepthHasStencil () and nothing
      else, and that is RG_FormatHasStencil (rgf_depth_fmt): the two packed
      formats, which rg_frame.c tries first, in that order, for the scene's
      depth target;
    * the stencil seam's GL arms are the bare calls, RP_GL (glEnable_fp /
      glDisable_fp (GL_STENCIL_TEST)), RP_GL (glStencilFunc_fp (func, ref,
      mask)), RP_GL (glStencilOp_fp (sfail, dpfail, dppass)), and
      gl_pipeline.c never calls glStencilMask or a *Separate (the -gpu arm
      writes glStencilMask's default 0xFF to both faces; every other file is
      kept off all stencil state by tools/test_draw_state_routing.py);
    * rg_pipeline.c maps every GL compare func and stencil op to its own
      SDL_GPU value, keys func, ops and compare mask from the shadow only
      where the depth format has stencil, never keys the reference, and
      builds one SDL_GPUStencilOpState for both faces; rg_frame.c records
      the shadow's reference per draw and sets it with
      SDL_SetGPUStencilReference, and a stencil clear becomes the pass's
      stencil load op with RG_Clear's value.
  buffers, vertex arrays and indexed draws under -gpu (#340 P2d), the world,
  brush-model and sky VBO paths, which both sweeps draw:
    * each seam -- gl_buffer.c's GL_BindBuffer, R_GenBuffers,
      R_DeleteBuffers, R_BufferData, R_BufferSubData, R_GenVertexArrays,
      R_DeleteVertexArrays, R_BindVertexArray, R_Enable/
      DisableVertexAttribArray, R_VertexAttribPointer, R_VertexAttribIPointer,
      R_VertexAttrib4f and gl_vbo.c's R_DrawElements / Instanced -- is
      exactly `#ifdef USE_SDL_GPU / if (R_GPU ()) { RG_x (...); return; } /
      #endif / glX_fp (...);` with the seam's own parameters, so GL is
      call-for-call what the call sites made (BUFFER_SEAMS);
    * R_BuildWorldVBO and R_BuildSkyStencilVBO no longer test R_GPU (),
      make no raw GL call, and make their VAO and buffers through the seams;
    * rg_buffer.c: a respecify (glBufferData) ends a segment that reads the
      buffer and queues the whole store with cycling on, a sub-update ends
      it and queues without, each upload is issued with the cycle it was
      queued with, and the disabled standard inputs ATTR_POSITION ..
      ATTR_COLOR are an instance-rate FLOAT4 slot of glVertexAttrib4f's
      values; rg_frame.c takes the index width from GL's type (2 / 4 bytes),
      the first index from the byte offset divided by it, records both and
      replays them, and draws with the pipeline of the VAO's interned
      layout; rg_pipeline.c keys the pipeline on that layout id and builds
      the vertex input from it; the hazard's other half -- a recorded draw,
      only once recorded, stamps its element and vertex buffers -- a delete
      that lets go of the name in every VAO, the upload list's tail
      re-derived after the name table moves, retired buffers released at
      submit (BUFFER_PINS).  tools/test_buffer_routing.py keeps every other
      file off the raw calls; tools/test_rg_buffer_shadow.py runs
      rg_buffer.c itself against a fake SDL_GPU.
  storage buffers under -gpu (#340 P2e), what instanced and skeletal alias
  models read, which tools/sweep-alias-gpu.steps draws:
    * gl_buffer.c's GL_BindBufferBase, GL_BindBufferRange,
      GL_BindBuffersRange and GL_Upload have an SDL_GPU arm ahead of every
      GL call (SEAMS);
    * rg_shader.c's block-name table (RG_StoragePoint) is exactly the GL
      binding point of every storage block a graphics stage in
      engine/shaders declares -- VERT_SSBO / FRAG_SSBO's `gl`, or a plain
      layout(std430, binding = N) -- each name one point everywhere, and
      no entry without a declaration; RG_CheckStage numbers each block's
      slot past the stage's sampled and storage textures;
    * rg_buffer.c makes every buffer with graphics storage-read usage,
      refuses a storage range at a non-zero offset (SDL_GPU binds whole
      buffers) and hands GL_Upload's arm back at offset 0, queued with
      cycling on; rg_frame.c binds each storage slot to RG_Buf_Storage's
      buffer for the block's point, or the stand-in, and replays the
      draw's own slots;
    * GL_Init_NullGL turns on shader_storage and skeletal_animation and no
      other capability: compute_shaders stays false, so the light cluster,
      culling, SSAO and GPU particles (P5) stay off;
    * GL_DrawAliasSkeletal and use_instancing test shader_storage alone,
      with no -gpu note left behind.
  liquids and the translucent world under -gpu (#340 P3a), which
  tools/sweep-liquid-gpu.steps draws: they go through GL_ImmEnd and the P2
  seams, and three GL-only paths stay off under -gpu, each by
  `#ifdef USE_SDL_GPU / if (R_GPU ()) return[ false];` at the body's own
  level, ahead of the first thing the path does (GPU_GATES):
    * OIT (P6): OIT_CreateFBO, OIT_BeginTranslucency, OIT_EndTranslucency
      and OIT_Active.  The OIT twins read their targets through
      sampler2DMS, which -gpu drops; once P4 turns post-process on under
      -gpu, r_oit 1 would otherwise lose every translucent;
    * the soft-particle depth snapshot (P4): GL_SoftDepth_Capture returns
      after clearing soft_depth_valid, so sprites never sample a depth
      texture the null glBlitFramebuffer (not NULL) never wrote;
    * the GPU particle buffers: R_GPU_Particles_Init makes none, where it
      made raw GL into the null table for an SSBO path nothing draws.
  and one SDL workaround the sweep's map changes found: rg_resource.c's
  RGT_CreateLevels creates a 3D texture with the levels GL defined, not the
  full chain -- SDL 3.4's Vulkan defragmenter copies every level of a 3D
  texture at level 0's depth, out of bounds past level 0.
  particles, sprites and glows under -gpu (#340 P3b), which
  tools/sweep-particles-gpu.steps draws (check_points):
    * gl_particles 0 draws GL_POINTS at GL_SetPointSize's one size, a no-op
      under -gpu.  spart_vert.glsl writes that size to gl_PointSize under
      `#ifdef VULKAN` alone (GL keeps glPointSize, and its preprocessed program
      is unchanged), and
      every point draw is R_DrawParticles_Real's, on the two programs whose
      vertex stage is spart_vert; a second size needs a uniform instead.  The
      draw pin is textual: GL_POINTS is 0, and a draw whose mode reaches 0
      without spelling it would get a POINTLIST pipeline unseen;
    * testparticles and testentity, the sweep's fixed content, are behind
      CL_TestFixtureAllowed (noclip's conditions plus a server in this
      process), which runs before either command touches anything.
  fog under -gpu (#340 P3c), which tools/sweep-fog-gpu.steps draws
  (check_fog).  Fog is uniforms only -- FragParams' u_ffogv and u_fskyfog,
  which R_FlushUniforms hands SDL_GPU like any block -- so the pin is that it
  stays that way: no shader stage reads fog inside a VULKAN, GL_SPIRV or
  __VERSION__ conditional, and a fogged program's stages hold directives only
  there (spart_vert's gl_PointSize the one statement); v_fogdist sits at one
  location per program family (Vulkan links varyings by location); the five
  fogged programs keep their blend; FragParams keeps its fog members, is
  registered with the backend whole and snapshotted whole; R_SetFog and
  R_SetSkyFog write it on both backends; gl_fog.c never names the backend;
  and no engine C function that touches fog (the flat fog passes, r_fastsky's
  colour, the glows' attenuation, the additive black fog, every R_SetFog /
  R_SetWorldFrag caller) names the backend or is compiled under a backend
  conditional.
  the post-process chain's GL calls through seams (#340 P4a-1,
  check_postprocess), the chain still off under -gpu:
    * gl_fbo.c is a seam (SEAMS), in the client's source list beside
      gl_texbind.c, each of its GL arms is the one bare call it is named
      for, its parameters passed through in order, and each SDL_GPU arm notes
      the call and answers neutrally until P4a-2 (zero names,
      GL_FRAMEBUFFER_UNSUPPORTED, false, 0);
    * no gl*_fp call is left in gl_postprocess.c outside the OIT functions
      (P6) and the GPU palette-LUT builder (compute, P5), except where an
      SDL_GPU arm excludes it and PP_RAW_OK's few, each counted: MSAA's
      multisampled storage (P6), bloom's bufferless draws (P4e), and
      shutdown's program and buffer deletes; a function GPU_GATES gates whole
      gets no credit for its gate, so GL_SoftDepth_Capture is routed for P4b;
    * the gates: PP_NeedsPostProcess still returns false under -gpu (P4a-2
      lifts it with the backend), OIT_InitShader returns false (GPU_GATES),
      PP_CreateFBO forces 0 samples after the gl_max_samples clamp and before
      the MSAA branch, and an FBO failure under -gpu returns from BeginFrame
      before the copyback fallback turns post-process on; the soft-particle
      snapshot is valid only if R_BlitFramebuffer says the copy was made;
    * PpVert (pp_vert_mvp, float[16]) and PpParams (pp_params) are registered
      as mirrors in PP_InitShader's SDL_GPU arm, PP_PushParams flushes both
      after GL_GetMVP and returns, PP_BindUniformBlock returns under -gpu
      before any GL call, and the blocks registered engine-wide leave room
      under RG_MAX_BLOCKS for P4e's BloomParams.

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
    # framebuffers and renderbuffers (#340 P4a-1)
    "engine/h2shared/gl_fbo.c": None,
    "engine/h2shared/gl_pipeline.c": None,
    "engine/h2shared/gl_uniforms.c": None,
    "engine/h2shared/gl_vbo.c": None,
    # The rest of gl_shader.c is GL program set-up the seam calls into;
    # only the creation entry point and the sampler-unit pair (#340 P2a)
    # are the seam.
    "engine/h2shared/gl_shader.c": {"R_Program_Create", "R_ProgramSamplerLoc",
                                    "R_ProgramSamplerUnit"},
    # The rest of gl_buffer.c is the GL 4.3 frame ring and the indexed
    # binding cache, which -gpu leaves on the null table; the buffer and
    # vertex-array entry points (#340 P2d), the indexed bindings and
    # GL_Upload (P2e) are the seam.
    "engine/h2shared/gl_buffer.c": {"GL_BindBuffer", "R_GenBuffers", "R_DeleteBuffers", "R_BufferData",
                                    "R_BufferSubData", "R_GenVertexArrays", "R_DeleteVertexArrays",
                                    "R_BindVertexArray", "R_EnableVertexAttribArray",
                                    "R_DisableVertexAttribArray", "R_VertexAttribPointer",
                                    "R_VertexAttribIPointer", "R_VertexAttrib4f",
                                    # the indexed bindings and the stream (#340 P2e)
                                    "GL_BindBufferBase", "GL_BindBufferRange", "GL_BindBuffersRange",
                                    "GL_Upload"},
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
    # `if (...) stmt [else stmt]`, as an arm's else often is: the whole
    # statement, not up to the first `;` inside its body.
    m = re.match(r"if\s*\(", s[i:])
    if m:
        depth, j = 0, i + m.end() - 1
        for j in range(j, len(s)):
            depth += {"(": 1, ")": -1}.get(s[j], 0)
            if depth == 0:
                break
        e = stmt_end(s, j + 1)
        k = skip_ws_pp(s, e)
        if re.match(r"else\b", s[k:]):
            e = stmt_end(s, k + 4)
        return e
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
    if not (root / rel).is_file():
        fail("%s: missing -- the #340 check that pins %s has nothing to read" % (rel, name))
        return None
    code = strip_c_comments((root / rel).read_text(errors="replace"))
    found = [code[b:e] for n, b, e in functions(code) if n == name]
    if len(found) <= nth:
        fail("%s: no %s -- the #340 check that pins it has nothing to read" % (rel, name))
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

    # The world and sky VBOs are built under -gpu too, through gl_buffer.c's
    # seams (#340 P2d): no R_GPU () return, no raw GL, the objects each path
    # draws from made by the seams.
    rs = "engine/hexen2/gl_rsurf.c"
    for fn, want in (("R_BuildWorldVBO", ("R_GenVertexArrays(1, &world_vao);", "R_GenBuffers(1, &world_vbo);",
                                          "R_GenBuffers(1, &world_ibo);")),
                     ("R_BuildSkyStencilVBO", ("R_GenVertexArrays(1, &sky_stencil_vao);",
                                               "R_GenBuffers(1, &sky_stencil_vbo);",
                                               "R_GenBuffers(1, &sky_stencil_ibo);"))):
        b = body_named(root, rs, fn)
        if b is None:
            continue
        if re.search(r"\bR_GPU\s*\(", b):
            fail("%s: %s tests R_GPU () -- since P2d -gpu builds the VBO through the seams, and the "
                 "world, brush entities and sky draw from it as on GL (#340 P2d)" % (rs, fn))
        if re.search(r"\bgl[A-Z]\w*_fp\s*\(", b):
            fail("%s: %s makes a raw GL call -- buffers and vertex state go through gl_buffer.c's "
                 "seams, or -gpu never sees them (#340 P2d)" % (rs, fn))
        for w in want:
            if w not in b:
                fail("%s: %s no longer makes its objects through the seams, `%s` (#340 P2d)" % (rs, fn, w))
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
             "models read storage buffers (#340 P2b)" % rm)
    b = body_named(root, rm, "GL_DrawAliasSkeletal")
    if b is not None:
        g = re.search(r"if\s*\(\s*!\s*gl_renderer_caps\.shader_storage\s*\)\s*"
                      r"(?:return\s*;|\{[^{}]*\breturn\s*;\s*\})", b)
        f = re.search(r"\bGL_GetAliasGPUMesh\s*\(", b)
        if not g or not f or g.start() > f.start():
            fail("%s: GL_DrawAliasSkeletal must return without shader storage before it touches the "
                 "mesh -- its bones are a storage buffer (#340 P2b)" % rm)
        if re.search(r"\bRG_NoteUnhandled\s*\(", b):
            fail("%s: GL_DrawAliasSkeletal still notes skeletal models as undrawn -- -gpu draws them "
                 "since P2e" % rm)
    return 0


def norm(s):
    return re.sub(r"\s+", " ", s).strip()


def case_map(body, prefix):
    """{GL_X: SDL_..._Y} from `case GL_X: return <prefix>Y;` lines."""
    return dict(re.findall(r"case\s+(GL_\w+)\s*:\s*return\s+(" + prefix + r"\w+)\s*;", body))


COMPARE_MAP = {
    "GL_NEVER": "SDL_GPU_COMPAREOP_NEVER", "GL_LESS": "SDL_GPU_COMPAREOP_LESS",
    "GL_EQUAL": "SDL_GPU_COMPAREOP_EQUAL", "GL_LEQUAL": "SDL_GPU_COMPAREOP_LESS_OR_EQUAL",
    "GL_GREATER": "SDL_GPU_COMPAREOP_GREATER", "GL_NOTEQUAL": "SDL_GPU_COMPAREOP_NOT_EQUAL",
    "GL_GEQUAL": "SDL_GPU_COMPAREOP_GREATER_OR_EQUAL", "GL_ALWAYS": "SDL_GPU_COMPAREOP_ALWAYS",
}
STENCIL_OP_MAP = {
    "GL_KEEP": "SDL_GPU_STENCILOP_KEEP", "GL_ZERO": "SDL_GPU_STENCILOP_ZERO",
    "GL_REPLACE": "SDL_GPU_STENCILOP_REPLACE", "GL_INCR": "SDL_GPU_STENCILOP_INCREMENT_AND_CLAMP",
    "GL_DECR": "SDL_GPU_STENCILOP_DECREMENT_AND_CLAMP", "GL_INVERT": "SDL_GPU_STENCILOP_INVERT",
    "GL_INCR_WRAP": "SDL_GPU_STENCILOP_INCREMENT_AND_WRAP",
    "GL_DECR_WRAP": "SDL_GPU_STENCILOP_DECREMENT_AND_WRAP",
}
STENCIL_FORMATS = ["SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT", "SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT"]


def check_stencil(root):
    """#340 P2c: stencil under -gpu -- have_stencil, the scene's depth
    format, the seam's GL arms, and the -gpu arm's pipeline key, reference
    and clear."""
    vid = "engine/h2shared/gl_vidsdl.c"
    b = body_named(root, vid, "GL_Init_NullGL")
    if b is not None:
        hs = [norm(v) for v in re.findall(r"\bhave_stencil\s*=\s*([^;=][^;]*);", b)]
        if hs != ["RG_DepthHasStencil ()"]:
            fail("%s: GL_Init_NullGL must set have_stencil = RG_DepthHasStencil () and nothing else "
                 "(found %s) -- without it -gpu skips the sky pre-pass and draws the skybox over "
                 "whatever sky brushes hide (#340 P2c)" % (vid, hs or "none"))

    rf = "engine/h2shared/rg_frame.c"
    frame = strip_c_comments((root / rf).read_text())
    b = body_named(root, rf, "RG_FormatHasStencil")
    if b is not None:
        named = re.findall(r"SDL_GPU_TEXTUREFORMAT_\w+", b)
        if sorted(named) != sorted(STENCIL_FORMATS):
            fail("%s: RG_FormatHasStencil must name exactly the packed formats %s (found %s) (#340 P2c)"
                 % (rf, " and ".join(STENCIL_FORMATS), named))
    b = body_named(root, rf, "RG_DepthHasStencil")
    if b is not None and not re.search(r"\breturn\s+rg_dev\s*!=\s*NULL\s*&&\s*RG_FormatHasStencil\s*\(\s*"
                                       r"rgf_depth_fmt\s*\)\s*;", b):
        fail("%s: RG_DepthHasStencil must be `return rg_dev != NULL && RG_FormatHasStencil "
             "(rgf_depth_fmt);` -- the format the scene's depth target is made in (#340 P2c)" % rf)
    m = re.search(r"\brgf_depth_formats\s*\[\s*\]\s*=\s*\{([^}]*)\}", frame)
    if not m or [x.strip() for x in m.group(1).split(",") if x.strip()][:2] != STENCIL_FORMATS:
        fail("%s: rgf_depth_formats must try %s first, in that order: SDL guarantees one of them, "
             "and the sky needs the scene's stencil (#340 P2c)" % (rf, " then ".join(STENCIL_FORMATS)))
    b = body_named(root, rf, "RG_Frame_Init")
    if b is not None and not re.search(r"\brgf_depth_fmt\s*=\s*rgf_depth_formats\s*\[\s*i\s*\]\s*;", b):
        fail("%s: RG_Frame_Init must take the scene's depth format from rgf_depth_formats (#340 P2c)" % rf)
    b = body_named(root, rf, "RGF_EnsureScene")
    if b is not None and not re.search(r"\brgf_depth\s*=\s*RGF_MakeTarget\s*\(\s*rgf_depth_fmt\s*,", b):
        fail("%s: RGF_EnsureScene must make the depth target in rgf_depth_fmt (#340 P2c)" % rf)
    # Every draw is committed through RGF_CommitDraw (#340 P2d), which records
    # the dynamic state the pipeline does not carry.
    b = body_named(root, rf, "RGF_CommitDraw")
    if b is not None and not re.search(r"\bd->stencil_ref\s*=\s*\(Uint8\)\s*CLAMP\s*\(\s*0\s*,\s*ps->stencil_ref\s*,"
                                       r"\s*255\s*\)\s*;", b):
        fail("%s: RGF_CommitDraw must record the shadow's reference, `d->stencil_ref = (Uint8) CLAMP (0, "
             "ps->stencil_ref, 255);` -- GL clamps it to the buffer's 8 bits (#340 P2c)" % rf)
    for fn in ("RG_DrawImm", "RG_DrawIndexed"):
        b = body_named(root, rf, fn)
        if b is not None and not re.search(r"\bRGF_CommitDraw\s*\(\s*d\s*,\s*ps\s*\)\s*;\s*$", b):
            fail("%s: %s must end in RGF_CommitDraw (d, ps), which records its viewport, depth range "
                 "and stencil reference (#340 P2c, P2d)" % (rf, fn))
    b = body_named(root, rf, "RGF_Replay")
    if b is not None and not re.search(r"if\s*\(\s*d->stencil_ref\s*!=\s*ref\s*\)\s*\{\s*ref\s*=\s*d->stencil_ref\s*;"
                                       r"\s*SDL_SetGPUStencilReference\s*\(\s*pass\s*,\s*\(Uint8\)\s*ref\s*\)\s*;", b):
        fail("%s: RGF_Replay must set each draw's reference, SDL_SetGPUStencilReference (pass, "
             "(Uint8) ref) whenever d->stencil_ref changes -- the pipeline does not carry it (#340 P2c)" % rf)
    b = body_named(root, rf, "RGF_EndSegment")
    if b is not None:
        if not re.search(r"\bds\.stencil_load_op\s*=\s*\(\s*\(\s*rgf_clear_mask\s*&\s*GL_STENCIL_BUFFER_BIT\s*\)\s*"
                         r"\|\|\s*!\s*rgf_cleared\s*\)\s*\?\s*SDL_GPU_LOADOP_CLEAR\s*:\s*SDL_GPU_LOADOP_LOAD\s*;", b):
            fail("%s: the stencil load op must clear when a clear asked for stencil or the frame has not "
                 "started, `ds.stencil_load_op = ((rgf_clear_mask & GL_STENCIL_BUFFER_BIT) || "
                 "!rgf_cleared) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;` (#340 P2c)" % rf)
        if [norm(v) for v in re.findall(r"\bclear_stencil\s*=\s*([^;]+);", b)] != ["rgf_clear_stencil"]:
            fail("%s: the render pass must clear stencil to what RG_Clear was handed, "
                 "`ds.clear_stencil = rgf_clear_stencil;` alone (#340 P2c)" % rf)
        if not re.search(r"\bds\.stencil_store_op\s*=\s*SDL_GPU_STOREOP_STORE\s*;", b):
            fail("%s: the stencil must be stored, ds.stencil_store_op = SDL_GPU_STOREOP_STORE -- the "
                 "pre-pass and the skybox can land in different passes (#340 P2c)" % rf)
    b = body_named(root, rf, "RG_Clear")
    if b is not None:
        km = re.search(r"\bmask\s*&=\s*([^;]*);", b)
        if not km or "GL_STENCIL_BUFFER_BIT" not in km.group(1):
            fail("%s: RG_Clear must keep GL_STENCIL_BUFFER_BIT in the mask it records (#340 P2c)" % rf)
        if not re.search(r"if\s*\(\s*mask\s*&\s*GL_STENCIL_BUFFER_BIT\s*\)\s*rgf_clear_stencil\s*=\s*"
                         r"\(Uint8\)\s*stencil\s*;", b):
            fail("%s: RG_Clear must take the stencil value it is handed, `if (mask & "
                 "GL_STENCIL_BUFFER_BIT) rgf_clear_stencil = (Uint8) stencil;` (#340 P2c)" % rf)

    gp = "engine/h2shared/gl_pipeline.c"
    arms = {
        "R_SetStencilTest": [r"RP_GL\s*\(\s*glEnable_fp\s*\(\s*GL_STENCIL_TEST\s*\)\s*\)\s*;",
                             r"RP_GL\s*\(\s*glDisable_fp\s*\(\s*GL_STENCIL_TEST\s*\)\s*\)\s*;"],
        "R_SetStencilFunc": [r"RP_GL\s*\(\s*glStencilFunc_fp\s*\(\s*func\s*,\s*ref\s*,\s*mask\s*\)\s*\)\s*;"],
        "R_SetStencilOp": [r"RP_GL\s*\(\s*glStencilOp_fp\s*\(\s*sfail\s*,\s*dpfail\s*,\s*dppass\s*\)\s*\)\s*;"],
    }
    for fn, rxs in arms.items():
        b = body_named(root, gp, fn)
        if b is None:
            continue
        if len(re.findall(r"\bgl\w+_fp\s*\(", b)) != len(rxs) or not all(re.search(r, b) for r in rxs):
            fail("%s: %s's GL arm must be the bare call%s, %s (#340 P2c)"
                 % (gp, fn, "s" if len(rxs) > 1 else "", " / ".join(r.replace("\\s*", "") for r in rxs)))
    shadow = {"R_SetStencilFunc": (("stencil_func", "func"), ("stencil_ref", "ref"), ("stencil_mask", "mask")),
              "R_SetStencilOp": (("stencil_sfail", "sfail"), ("stencil_dpfail", "dpfail"),
                                 ("stencil_dppass", "dppass"))}
    for fn, pairs in shadow.items():
        b = body_named(root, gp, fn)
        if b is None:
            continue
        for f, a in pairs:
            if not re.search(r"\brp\.%s\s*=\s*%s\s*;" % (f, a), b):
                fail("%s: %s must shadow rp.%s = %s -- the shadow is the -gpu arm's whole draw state "
                     "(#340 P2c)" % (gp, fn, f, a))
    gpc = strip_c_comments((root / gp).read_text())
    m = re.search(r"\bglStencil(?:Mask|FuncSeparate|OpSeparate)\w*\s*\(", gpc)
    if m:
        fail("%s: %s -- the -gpu arm writes glStencilMask's default 0xFF to both faces; a write mask "
             "or a per-face stencil needs its own shadow and key first (#340 P2c)" % (gp, m.group(0)))

    rp = "engine/h2shared/rg_pipeline.c"
    for fn, want, prefix in (("RG_CompareOp", COMPARE_MAP, "SDL_GPU_COMPAREOP_"),
                             ("RG_StencilOp", STENCIL_OP_MAP, "SDL_GPU_STENCILOP_")):
        b = body_named(root, rp, fn)
        if b is None:
            continue
        got = case_map(b, prefix)
        if got != want:
            bad = sorted(set(want.items()) ^ set(got.items()))
            fail("%s: %s must map each GL value to its own SDL_GPU one; differs at %s (#340 P2c)"
                 % (rp, fn, bad[:4]))
    pipe = strip_c_comments((root / rp).read_text())
    km = re.search(r"typedef\s+struct\s*\{([^}]*)\}\s*rgpipekey_t\s*;", pipe)
    if not km or re.search(r"\b\w*ref\b", km.group(1)):
        fail("%s: rgpipekey_t must not carry the stencil reference -- it is dynamic state "
             "(SDL_SetGPUStencilReference), and keying it would multiply the pipelines (#340 P2c)" % rp)
    b = body_named(root, rp, "RG_MakeKey")
    if b is not None:
        if not re.search(r"if\s*\(\s*s->stencil_test\s*&&\s*RG_FormatHasStencil\s*\(\s*depth\s*\)\s*\)", b):
            fail("%s: RG_MakeKey must key the stencil when the shadow tests it and the depth format has "
                 "stencil, `if (s->stencil_test && RG_FormatHasStencil (depth))` (#340 P2c)" % rp)
        for f, src in (("stencil_func", r"s->stencil_func"), ("stencil_mask", r"s->stencil_mask\s*&\s*0xFF"),
                       ("sfail", r"s->stencil_sfail"), ("dpfail", r"s->stencil_dpfail"),
                       ("dppass", r"s->stencil_dppass")):
            if len(re.findall(r"\bk->%s\s*=" % f, b)) != 1 or not re.search(r"\bk->%s\s*=\s*%s\s*;" % (f, src), b):
                fail("%s: RG_MakeKey must set k->%s from the shadow, once (#340 P2c)" % (rp, f))
    b = body_named(root, rp, "RG_CreatePipeline")
    if b is not None:
        need = [
            (r"\bst\.compare_op\s*=\s*RG_CompareOp\s*\(\s*k->stencil_func\s*\)\s*;", "st.compare_op = RG_CompareOp (k->stencil_func)"),
            (r"\bst\.fail_op\s*=\s*RG_StencilOp\s*\(\s*k->sfail\s*\)\s*;", "st.fail_op = RG_StencilOp (k->sfail)"),
            (r"\bst\.depth_fail_op\s*=\s*RG_StencilOp\s*\(\s*k->dpfail\s*\)\s*;", "st.depth_fail_op = RG_StencilOp (k->dpfail)"),
            (r"\bst\.pass_op\s*=\s*RG_StencilOp\s*\(\s*k->dppass\s*\)\s*;", "st.pass_op = RG_StencilOp (k->dppass)"),
            (r"\bfront_stencil_state\s*=\s*st\s*;", "ci.depth_stencil_state.front_stencil_state = st"),
            (r"\bback_stencil_state\s*=\s*st\s*;", "ci.depth_stencil_state.back_stencil_state = st"),
            (r"\benable_stencil_test\s*=\s*true\s*;", "ci.depth_stencil_state.enable_stencil_test = true"),
            (r"\bcompare_mask\s*=\s*\(Uint8\)\s*k->stencil_mask\s*;", "ci.depth_stencil_state.compare_mask = (Uint8) k->stencil_mask"),
            (r"\bwrite_mask\s*=\s*0xFF\s*;", "ci.depth_stencil_state.write_mask = 0xFF"),
        ]
        for rx, what in need:
            if not re.search(rx, b):
                fail("%s: RG_CreatePipeline must set `%s;` (#340 P2c)" % (rp, what))
    return 0


# The buffer, vertex-array and indexed-draw seams (#340 P2d): function ->
# (file, its GL arm, its SDL_GPU arm), each the one call, with the seam's own
# parameters in GL's order.
BUFFER_SEAMS = {
    "GL_BindBuffer": ("gl_buffer.c", "glBindBuffer_fp (target, buffer)", "RG_BindBuffer (target, buffer)"),
    "R_GenBuffers": ("gl_buffer.c", "glGenBuffers_fp (n, buffers)", "RG_GenBuffers (n, buffers)"),
    "R_DeleteBuffers": ("gl_buffer.c", "glDeleteBuffers_fp (n, buffers)", "RG_DeleteBuffers (n, buffers)"),
    "R_BufferData": ("gl_buffer.c", "glBufferData_fp (target, size, data, usage)",
                     "RG_BufferData (target, size, data, usage)"),
    "R_BufferSubData": ("gl_buffer.c", "glBufferSubData_fp (target, offset, size, data)",
                        "RG_BufferSubData (target, offset, size, data)"),
    "R_GenVertexArrays": ("gl_buffer.c", "glGenVertexArrays_fp (n, arrays)", "RG_GenVertexArrays (n, arrays)"),
    "R_DeleteVertexArrays": ("gl_buffer.c", "glDeleteVertexArrays_fp (n, arrays)",
                             "RG_DeleteVertexArrays (n, arrays)"),
    "R_BindVertexArray": ("gl_buffer.c", "glBindVertexArray_fp (array)", "RG_BindVertexArray (array)"),
    "R_EnableVertexAttribArray": ("gl_buffer.c", "glEnableVertexAttribArray_fp (index)",
                                  "RG_EnableVertexAttribArray (index, true)"),
    "R_DisableVertexAttribArray": ("gl_buffer.c", "glDisableVertexAttribArray_fp (index)",
                                   "RG_EnableVertexAttribArray (index, false)"),
    "R_VertexAttribPointer": ("gl_buffer.c",
                              "glVertexAttribPointer_fp (index, size, type, normalized, stride, pointer)",
                              "RG_VertexAttribPointer (index, size, type, normalized, false, stride, pointer)"),
    "R_VertexAttribIPointer": ("gl_buffer.c", "glVertexAttribIPointer_fp (index, size, type, stride, pointer)",
                               "RG_VertexAttribPointer (index, size, type, GL_FALSE, true, stride, pointer)"),
    "R_VertexAttrib4f": ("gl_buffer.c", "glVertexAttrib4f_fp (index, x, y, z, w)",
                         "RG_VertexAttrib4f (index, x, y, z, w)"),
    "R_DrawElements": ("gl_vbo.c", "glDrawElements_fp (mode, count, type, indices)",
                       "RG_DrawIndexed (mode, count, type, indices, 1)"),
    "R_DrawElementsInstanced": ("gl_vbo.c",
                                "glDrawElementsInstanced_fp (mode, count, type, indices, instancecount)",
                                "RG_DrawIndexed (mode, count, type, indices, instancecount)"),
}

# rg_*.c lines P2d rests on: (file, function or None for the file, regex, why).
BUFFER_PINS = (
    ("rg_buffer.c", "RG_BufferData", r"\bRGB_FlushIfRead\s*\(\s*b\s*\)\s*;\s*RGB_DropUploads\s*\(\s*b\s*\)\s*;"
     r".*\bRGB_Queue\s*\(\s*b\s*,\s*0\s*,\s*\(Uint32\)\s*size\s*,\s*data\s*,\s*true\s*\)\s*;",
     "a respecify ends a segment that reads the buffer before it drops or re-targets anything, then "
     "queues the whole store with cycling on: draws still in flight keep the old contents"),
    ("rg_buffer.c", "RGB_FlushIfRead", r"^\s*\{\s*if\s*\(\s*b->read_ever\s*&&\s*b->read_segment\s*==\s*"
     r"RG_Frame_Segment\s*\(\s*\)\s*&&\s*RG_Frame_SegmentHasDraws\s*\(\s*\)\s*\)\s*RG_Frame_Flush\s*\(\s*\)\s*;\s*$",
     "the hazard: a buffer a draw of the open segment reads ends the segment"),
    ("rg_buffer.c", "RG_Buf_ForDraw", r"\bin->reads\[in->nreads\+\+\]\s*=\s*b\s*;.*"
     r"\bin->reads\[in->nreads\+\+\]\s*=\s*used\[s\]\s*;",
     "the draw's element buffer and every vertex buffer are the ones it marks read"),
    ("rg_buffer.c", "RG_Buf_MarkRead", r"for\s*\(\s*i\s*=\s*0\s*;\s*i\s*<\s*in->nreads\s*;\s*i\+\+\s*\)\s*"
     r"RGB_Stamp\s*\(\s*\(rgbuf_t\s*\*\)\s*in->reads\[i\]\s*\)\s*;",
     "marking a draw's buffers read stamps every one with the open segment"),
    ("rg_buffer.c", "RGB_Stamp", r"\bb->read_ever\s*=\s*true\s*;\s*b->read_segment\s*=\s*RG_Frame_Segment\s*\(\s*\)\s*;",
     "a stamp is the open segment"),
    ("rg_buffer.c", "RG_DeleteBuffers", r"for\s*\(\s*k\s*=\s*1\s*;\s*k\s*<=\s*rgb_vaonames\.used\s*;\s*k\+\+\s*\)"
     r"\s*\{\s*if\s*\(\s*RGB_VAOS\[k\]\.alive\s*\)\s*RGB_Detach\s*\(\s*&RGB_VAOS\[k\]\s*,\s*buffers\[i\]\s*\)\s*;",
     "a deleted buffer's name leaves every VAO: the name is reused, and a stale VAO would draw from its successor"),
    ("rg_buffer.c", "RGB_Queue", r"if\s*\(\s*!\s*b->uploads\s*\)\s*b->uploads_tail\s*=\s*&b->uploads\s*;\s*"
     r"\*b->uploads_tail\s*=\s*u\s*;",
     "an empty upload list's tail is re-derived: the name table may have moved under it"),
    ("rg_frame.c", "RG_DrawIndexed", r"\bd\s*=\s*RGF_BeginDraw\s*\(\s*prog\s*,\s*prim\s*,\s*layout\s*\)\s*;\s*"
     r"if\s*\(\s*!\s*d\s*\)\s*return\s*;\s*RG_Buf_MarkRead\s*\(\s*&in\s*\)\s*;",
     "a recorded draw, and only a recorded one, marks its buffers read"),
    ("rg_frame.c", "RGF_Submit", r"\bRG_Buf_EndFrame\s*\(\s*\)\s*;",
     "retired buffers are released after the frame's command buffer is submitted"),
    ("rg_buffer.c", "RG_BufferSubData", r"\bRGB_FlushIfRead\s*\(\s*b\s*\)\s*;\s*RGB_Queue\s*\(\s*b\s*,\s*"
     r"\(Uint32\)\s*offset\s*,\s*\(Uint32\)\s*size\s*,\s*data\s*,\s*false\s*\)\s*;",
     "a partial update ends a segment that reads the buffer, then queues without cycling: a cycled "
     "buffer loses the rest of its contents"),
    ("rg_buffer.c", "RG_Buf_IssueUploads", r"\bSDL_UploadToGPUBuffer\s*\(\s*copy\s*,\s*&src\s*,\s*&dst\s*,"
     r"\s*u->cycle\s*\)\s*;", "each upload cycles exactly when it was queued to"),
    ("rg_buffer.c", None, r"#define\s+RGB_CONST_FIRST\s+ATTR_POSITION\s*\n\s*#define\s+RGB_CONST_LAST\s+ATTR_COLOR\b",
     "every standard input, ATTR_POSITION .. ATTR_COLOR, has a value when its array is off"),
    ("rg_buffer.c", "RG_Buf_ForDraw", r"for\s*\(\s*i\s*=\s*RGB_CONST_FIRST\s*;\s*i\s*<=\s*RGB_CONST_LAST\s*;\s*i\+\+\s*\)"
     r"\s*\{[^{}]*?if\s*\(\s*v->attr\[i\]\.enabled\s*\)\s*continue\s*;",
     "the disabled standard inputs are fed their current value (glVertexAttrib4f): a vertex input the "
     "pipeline does not supply is an error"),
    ("rg_buffer.c", "RG_Buf_ForDraw", r"\bin->layout\.slot\[nslots\]\.instance\s*=\s*1\s*;.*"
     r"\bla->format\s*=\s*SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4\s*;.*"
     r"\bmemcpy\s*\(\s*in->consts\[in->nconst\]\s*,\s*rgb_current\[i\]\s*,",
     "the constants are an instance-rate FLOAT4 slot holding glVertexAttrib4f's values"),
    ("rg_frame.c", "RG_DrawIndexed", r"case\s+GL_UNSIGNED_SHORT\s*:\s*isize\s*=\s*2\s*;\s*break\s*;\s*"
     r"case\s+GL_UNSIGNED_INT\s*:\s*isize\s*=\s*4\s*;\s*break\s*;",
     "the index width is GL's type: 2 bytes for GL_UNSIGNED_SHORT, 4 for GL_UNSIGNED_INT"),
    ("rg_frame.c", "RG_DrawIndexed", r"\bfirst\s*=\s*\(Uint32\)\s*\(\s*\(uintptr_t\)\s*indices\s*/\s*isize\s*\)\s*;",
     "GL's byte offset becomes SDL_GPU's first index, divided by the index width"),
    ("rg_frame.c", "RG_DrawIndexed", r"\bd->ib_size\s*=\s*\(\s*isize\s*==\s*4\s*\)\s*\?\s*"
     r"SDL_GPU_INDEXELEMENTSIZE_32BIT\s*:\s*SDL_GPU_INDEXELEMENTSIZE_16BIT\s*;\s*d->first_index\s*=\s*first\s*;",
     "the recorded index size and first index are the ones computed from GL's type and offset"),
    ("rg_frame.c", "RG_DrawIndexed", r"\blayout\s*=\s*RG_LayoutIntern\s*\(\s*&in\.layout\s*\)\s*;\s*"
     r"d\s*=\s*RGF_BeginDraw\s*\(\s*prog\s*,\s*prim\s*,\s*layout\s*\)\s*;",
     "the pipeline is the one for this VAO's interned layout"),
    ("rg_frame.c", "RGF_Replay", r"\bSDL_BindGPUIndexBuffer\s*\(\s*pass\s*,\s*&ib\s*,\s*d->ib_size\s*\)\s*;\s*"
     r"SDL_DrawGPUIndexedPrimitives\s*\(\s*pass\s*,\s*d->count\s*,\s*d->instances\s*,\s*d->first_index\s*,",
     "the replay binds the recorded index size and draws from the recorded first index"),
    ("rg_pipeline.c", "RG_MakeKey", r"\bk->layout\s*=\s*layout\s*;",
     "the vertex layout id is part of the pipeline key: two layouts never share a pipeline"),
    ("rg_pipeline.c", "RG_CreatePipeline", r"\bconst\s+rgvlayout_t\s*\*\s*l\s*=\s*&rg_layouts\s*\[\s*k->layout\s*\]\s*;",
     "the pipeline's vertex input is the key's layout"),
)


def check_buffers(root):
    """#340 P2d: the buffer, vertex-array and indexed-draw seams' arms, and
    the backend lines the sweeps rest on."""
    for fn, (f, gl, rg) in BUFFER_SEAMS.items():
        rel = "engine/h2shared/" + f
        b = body_named(root, rel, fn)
        if b is None:
            continue
        glrx = re.escape(gl).replace(r"\ ", r"\s*").replace(r"\(", r"\s*\(\s*").replace(",", r"\s*,\s*")
        rgrx = re.escape(rg).replace(r"\ ", r"\s*").replace(r"\(", r"\s*\(\s*").replace(",", r"\s*,\s*")
        shape = (r"^\s*#\s*ifdef\s+USE_SDL_GPU\s*if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*\{\s*" + rgrx +
                 r"\s*;\s*return\s*;\s*\}\s*#\s*endif\s*" + glrx + r"\s*;\s*$")
        if not re.match(r"\s*\{", b) or not re.search(shape, b[1:]):
            fail("%s: %s must be `#ifdef USE_SDL_GPU / if (R_GPU ()) { %s; return; } / #endif / %s;` "
                 "-- the GL arm the one bare call, so GL is call-for-call unchanged (#340 P2d)"
                 % (rel, fn, rg, gl))
    # The GL 4.3 ring binds raw: through the seam, a GL_Upload to a vertex
    # target would write a null-table name into rg_buffer.c's bindings.
    for fn in ("GL_Upload", "GL_AllocFrameHostBuffers", "GL_DeleteFrameResources"):
        b = body_named(root, "engine/h2shared/gl_buffer.c", fn)
        if b is not None and re.search(r"\bGL_BindBuffer\s*\(", b):
            fail("engine/h2shared/gl_buffer.c: %s binds through GL_BindBuffer -- the ring is GL-only and "
                 "binds with the bare glBindBuffer_fp, or -gpu's buffer shadow takes its names (#340 P2d)" % fn)
    for f, fn, rx, why in BUFFER_PINS:
        rel = "engine/h2shared/" + f
        if fn is None:
            if not (root / rel).is_file():
                fail("%s: missing -- %s (#340 P2d)" % (rel, why))
                continue
            code = strip_c_comments((root / rel).read_text(errors="replace"))
        else:
            code = body_named(root, rel, fn)
            if code is None:
                continue
        if not re.search(rx, code, re.S):
            fail("%s: %s: %s (#340 P2d)" % (rel, fn or "file", why))
    return 0


# Storage blocks: VERT_SSBO / FRAG_SSBO (layout.inc), or a plain std430
# block with its GL binding spelled (spart_gpu_vert.glsl).
SSBO_DECL = re.compile(r"\b(?:VERT_SSBO|FRAG_SSBO)\s*\(\s*(\w+)\s*,\s*\w+\s*\)[^;{]*?\bbuffer\s+(\w+)\s*\{")
SSBO_PLAIN = re.compile(r"layout\s*\(\s*std430\s*,\s*binding\s*=\s*(\w+)\s*\)[^;{]*?\bbuffer\s+(\w+)\s*\{")
STORAGE_PINS = (
    ("rg_buffer.c", "RGB_Create", r"\bci\.usage\s*=\s*SDL_GPU_BUFFERUSAGE_VERTEX\s*\|\s*SDL_GPU_BUFFERUSAGE_INDEX\s*\|"
     r"\s*SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ\s*;",
     "every buffer is made with vertex, index and graphics storage-read usage: GL does not say which a "
     "name will be, and a storage slot bound to a buffer without it is a validation error"),
    ("rg_buffer.c", "RG_Buf_Storage", r"if\s*\(\s*p->offset\s*!=\s*0\s*\)\s*\{[^{}]*\breturn\s+NULL\s*;\s*\}",
     "a storage range at a non-zero offset is refused: SDL_GPU binds the buffer whole, and the "
     "shader would read from its start"),
    ("rg_buffer.c", "RG_Buf_Storage", r"\*read\s*=\s*b\s*;\s*return\s+b->buf\s*;",
     "a binding point's buffer is what the slot gets, and what the draw stamps as read"),
    ("rg_buffer.c", "RG_Upload", r"\bname\s*=\s*rgb_stream\[rgb_stream_used\+\+\]\s*;.*"
     r"\bRGB_Queue\s*\(\s*b\s*,\s*0\s*,\s*\(Uint32\)\s*numbytes\s*,\s*data\s*,\s*true\s*\)\s*;"
     r"\s*\*outbuf\s*=\s*name\s*;",
     "each upload lands at offset 0 of a buffer of its own, cycled: last frame's draws keep their bytes"),
    ("rg_buffer.c", "RG_BindBufferRange", r"\brgb_points\[index\]\.buffer\s*=\s*buffer\s*;\s*"
     r"rgb_points\[index\]\.offset\s*=\s*buffer\s*\?\s*offset\s*:\s*0\s*;",
     "the binding point records the buffer and the offset it was bound at"),
    ("rg_frame.c", "RGF_RecordStage", r"\bSDL_GPUBuffer\s*\*buf\s*=\s*RG_Buf_Storage\s*\(\s*sb->point\s*,\s*&read\s*\)\s*;"
     r".*\bslots\[sb->slot\]\s*=\s*buf\s*;.*\bslots\[sb->slot\]\s*=\s*rgf_ssbo\s*;",
     "each storage slot gets the buffer at its block's GL binding point, the stand-in when none"),
    ("rg_frame.c", "RGF_Replay", r"\bSDL_BindGPUVertexStorageBuffers\s*\(\s*pass\s*,\s*0\s*,\s*d->vssbo\s*,\s*d->nvssbo\s*\)\s*;"
     r".*\bSDL_BindGPUFragmentStorageBuffers\s*\(\s*pass\s*,\s*0\s*,\s*d->fssbo\s*,\s*d->nfssbo\s*\)\s*;",
     "the replay binds the draw's own storage slots"),
    ("rg_frame.c", "RGF_CommitDraw", r"\bRG_Buf_StampRead\s*\(",
     "a committed draw stamps the storage buffers it reads, so an upload into one ends the segment"),
    ("rg_shader.c", "RG_CheckStage", r"\bfirst\s*=\s*refl->samplers\s*\+\s*refl->ro_storage_textures\s*;.*"
     r"\bslot\s*=\s*\(int\)\s*out->ssbos\[i\]\.slot\s*-\s*first\s*;.*"
     r"\bout->ssbos\[i\]\.slot\s*=\s*\(Uint8\)\s*slot\s*;",
     "a storage block's slot is its binding past the stage's sampled and storage textures, and that "
     "is what the stage keeps (a fragment stage's raw binding counts its samplers)"),
)


def check_storage(root):
    """#340 P2e: storage buffers for instanced and skeletal alias models."""
    # The block-name table against every graphics stage's declarations.
    defs = {}
    for h in ("engine/h2shared/gl_lightcluster.h",):
        for m in re.finditer(r"^\s*#\s*define\s+(\w+)\s+(\d+)\b", (root / h).read_text(), re.M):
            defs[m.group(1)] = int(m.group(2))

    def value(tok, where):
        if tok.isdigit():
            return int(tok)
        if tok in defs:
            return defs[tok]
        fail("%s: storage binding %s is neither a number nor a gl_lightcluster.h #define" % (where, tok))
        return None
    declared = {}
    for p in sorted((root / "engine/shaders").glob("*")):
        if p.suffix not in (".glsl", ".inc") or p.name.endswith("_comp.glsl"):
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        code = re.sub(r"^\s*#\s*define\b.*$", "", code, flags=re.M)
        for rx in (SSBO_DECL, SSBO_PLAIN):
            for m in rx.finditer(code):
                v = value(m.group(1), p.name)
                prev = declared.setdefault(m.group(2), (v, p.name))
                if prev[0] != v:
                    fail("engine/shaders: storage block %s is GL binding %s in %s and %s in %s -- the "
                         "SDL_GPU backend finds the point by name (#340 P2e)"
                         % (m.group(2), prev[0], prev[1], v, p.name))
    if not declared:
        fail("engine/shaders: no storage block found -- the P2e table check has nothing to hold")
    rs = "engine/h2shared/rg_shader.c"
    code = strip_c_comments((root / rs).read_text(errors="replace"))
    m = re.search(r"\brg_storage_points\s*\[\s*\]\s*=\s*\{(.*?)\};", code, re.S)
    table = {}
    if not m:
        fail("%s: no rg_storage_points[] table (#340 P2e)" % rs)
    else:
        for e in re.finditer(r"\{\s*\"(\w+)\"\s*,\s*(\w+)\s*\}", m.group(1)):
            table[e.group(1)] = value(e.group(2), rs)
    for name, (v, where) in sorted(declared.items()):
        if name not in table:
            fail("%s: rg_storage_points has no %s (%s, GL binding %s) -- under -gpu its slot reads the "
                 "zeroed stand-in (#340 P2e)" % (rs, name, where, v))
        elif table[name] != v:
            fail("%s: rg_storage_points maps %s to %s, %s binds it at %s -- under -gpu the slot would "
                 "read another block's buffer (#340 P2e)" % (rs, name, table[name], where, v))
    for name in sorted(set(table) - set(declared)):
        fail("%s: rg_storage_points names %s, which no graphics stage declares -- drop it" % (rs, name))

    # Capabilities: storage on, compute off.
    b = body_named(root, "engine/h2shared/gl_vidsdl.c", "GL_Init_NullGL")
    if b is not None:
        on = sorted(set(re.findall(r"\bgl_renderer_caps\.(\w+)\s*=\s*true\s*;", b)))
        if on != ["shader_storage", "skeletal_animation"]:
            fail("engine/h2shared/gl_vidsdl.c: GL_Init_NullGL turns on %s; under -gpu exactly "
                 "shader_storage and skeletal_animation are real (#340 P2e) -- compute_shaders would "
                 "run the light cluster, culling and SSAO against the null table (P5)" % (on or "nothing"))
        other = [c for c in re.findall(r"\bgl_renderer_caps\.(\w+)\s*=\s*([^;]+);", b)
                 if c[1].strip() not in ("true", "false", "GL_RENDERER_DESKTOP_43", '"SDL_GPU"')]
        if other:
            fail("engine/h2shared/gl_vidsdl.c: GL_Init_NullGL sets %s from an expression -- every "
                 "capability there is a constant (#340 P2e)" % ", ".join(c[0] for c in other))

    for f, fn, rx, why in STORAGE_PINS:
        rel = "engine/h2shared/" + f
        body = body_named(root, rel, fn)
        if body is not None and not re.search(rx, body, re.S):
            fail("%s: %s: %s (#340 P2e)" % (rel, fn, why))
    return 0


# (file, function, what the gate returns, the first thing it must come before,
#  what must come before the gate or None, why).  #340 P3a.
GPU_GATES = (
    ("engine/h2shared/gl_postprocess.c", "OIT_CreateFBO", "false",
     r"\bOIT_DeleteFBO\s*\(|\bR_GenTextures\s*\(|\bgl\w+_fp\s*\(", None,
     "OIT is P6: its twins read sampler2DMS, which -gpu drops"),
    ("engine/h2shared/gl_postprocess.c", "OIT_BeginTranslucency", "",
     r"\boit_in_pass\s*=|\bgl\w+_fp\s*\(|\bR_Set\w+\s*\(", None,
     "OIT is P6: once P4 runs post-process under -gpu, r_oit 1 would draw every translucent into nothing"),
    ("engine/h2shared/gl_postprocess.c", "OIT_EndTranslucency", "",
     r"\boit_in_pass\s*=|\bgl\w+_fp\s*\(|\bR_\w+\s*\(", None,
     "OIT is P6: the resolve samples the twins' sampler2DMS targets"),
    ("engine/h2shared/gl_postprocess.c", "OIT_Active", "false",
     r"\breturn\s+oit_available\b", None,
     "OIT is P6: callers must see sorted blending under -gpu"),
    ("engine/h2shared/gl_postprocess.c", "OIT_InitShader", "false",
     r"\bR_Program_Create\s*\(|\bgl\w+_fp\s*\(|\bPP_BindUniformBlock\s*\(", None,
     "OIT is P6: the null glBlendFunci is not NULL, so every -gpu boot reached it, and it is raw GL "
     "for a resolve -gpu never draws (#340 P4a-1)"),
    ("engine/h2shared/gl_postprocess.c", "PP_NeedsPostProcess", "false",
     r"\bv_gamma\b|\bPP_Contrast\s*\(", r"\bif\s*\(\s*!\s*pp_initialized\s*\)\s*return\s+false\s*;",
     "the chain's calls go through seams (P4a-1) whose -gpu arms are not built yet: P4a-2 lifts this "
     "with the backend, and this row with it"),
    ("engine/h2shared/gl_postprocess.c", "GL_SoftDepth_Capture", "",
     r"\bR_GenTextures\s*\(|\bgl\w+_fp\s*\(|\bR_\w*(?:Framebuffer|Renderbuffer|DrawBuffers)\w*\s*\("
     r"|\bsoft_depth_valid\s*=\s*true\b",
     r"\bsoft_depth_valid\s*=\s*false\s*;",
     "soft particles are P4: the null glBlitFramebuffer is not NULL, so the snapshot would be marked "
     "valid and sprites would sample a texture nothing wrote"),
    ("engine/hexen2/r_part.c", "R_GPU_Particles_Init", "",
     r"\bGL_CreateParticleResources\s*\(", None,
     "the GPU particle buffers are raw GL, into the null table, for a path nothing draws"),
)


def check_translucency(root):
    """#340 P3a: the GL-only paths beside the liquids and translucents stay
    off under -gpu."""
    for rel, name, ret, first_rx, after_rx, why in GPU_GATES:
        if not (root / rel).is_file():
            fail("%s: missing -- the #340 P3a gate on %s has nothing to read" % (rel, name))
            continue
        code = strip_c_comments((root / rel).read_text(errors="replace"))
        bodies = [code[b:e] for n, b, e in functions(code) if n == name]
        # the real definition, not a tier's empty stub
        bodies = [b for b in bodies if re.search(first_rx, b)]
        if len(bodies) != 1:
            fail("%s: %d definitions of %s doing what the #340 P3a gate guards -- the check pins "
                 "exactly one" % (rel, len(bodies), name))
            continue
        body = bodies[0]
        gates = list(re.finditer(r"#\s*ifdef\s+USE_SDL_GPU\s*\n\s*if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*"
                                 r"return\s*%s\s*;\s*#\s*endif" % (re.escape(ret) if ret else ""), body))
        first = re.search(first_rx, body)
        ok = bool(gates) and first is not None and gates[0].start() < first.start()
        if ok:
            head = body[:gates[0].start()]
            ok = head.count("{") - head.count("}") == 1
            # and not inside a conditional the body opened (#if 0 hides it)
            opened = len(re.findall(r"^\s*#\s*if(?:def|ndef)?\b", head, re.M)) - \
                len(re.findall(r"^\s*#\s*endif\b", head, re.M))
            ok = ok and opened == 0
        if ok and after_rx:
            a = re.search(after_rx, body)
            ok = a is not None and a.end() <= gates[0].start()
        if not ok:
            want = "`#ifdef USE_SDL_GPU / if (R_GPU ()) return%s; / #endif`" % (" " + ret if ret else "")
            where = " after `%s`" % after_rx.replace("\\b", "").replace("\\s*", " ") if after_rx else ""
            fail("%s: %s needs %s%s at the body's own level, ahead of what it guards -- %s "
                 "(#340 P3a)" % (rel, name, want, where, why))
    rr = "engine/h2shared/rg_resource.c"
    b = body_named(root, rr, "RGT_CreateLevels")
    shape = (r"^\{\s*int\s+full\s*=\s*RGT_FullChain\s*\(\s*t\s*\)\s*;"
             r"\s*if\s*\(\s*t->usage\s*&\s*\(\s*SDL_GPU_TEXTUREUSAGE_COLOR_TARGET\s*\|\s*"
             r"SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET\s*\)\s*\)\s*return\s+q_min\s*\(\s*t->levels\s*,\s*full\s*\)\s*;"
             r"\s*if\s*\(\s*t->type\s*==\s*RG_TT_3D\s*\)\s*return\s+q_min\s*\(\s*t->levels\s*,\s*full\s*\)\s*;"
             r"\s*return\s+full\s*;\s*\}?$")
    if b is not None and not re.match(shape, b.strip()):
        fail("%s: RGT_CreateLevels must be exactly: the full chain, render targets and then 3D textures "
             "with the levels GL defined (`if (t->type == RG_TT_3D) return q_min (t->levels, full);`), "
             "everything else the full chain -- SDL 3.4.12's Vulkan defragmenter copies each level of a "
             "3D texture at level 0's depth, out of bounds past level 0 (#340 P3a)" % rr)
    # That workaround is only free while every 3D texture defines level 0
    # alone: a level defined after first use re-creates the texture and loses
    # its contents (counted in gpu_unrouted, caught by nothing else).
    for p in sorted((root / "engine").rglob("*.c")):
        if "rust" in p.parts:
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        for m in re.finditer(r"\bR_TexImage3D\s*\(\s*GL_TEXTURE_3D\s*,\s*[^,]+,\s*([^,]+?)\s*,", code):
            if m.group(1) != "0":
                fail("%s:%d: R_TexImage3D (GL_TEXTURE_3D, ...) at level `%s` -- under -gpu a 3D texture "
                     "gets only the levels it defines before first use (rg_resource.c "
                     "RGT_CreateLevels, the SDL 3.4.12 defragmenter workaround); a mipped 3D texture "
                     "needs that revisited first (#340 P3a)"
                     % (p.relative_to(root), code.count("\n", 0, m.start()) + 1, m.group(1)))

POINT_SIZE = re.compile(r"\bGL_SetPointSize\s*\(\s*([^)]*?)\s*\)")


def num(v):
    try:
        return float(v.rstrip("fF"))
    except ValueError:
        return None


def check_points(root):
    """#340 P3b: square particles keep their size under -gpu, and the sweep's
    test fixtures are cheat-gated."""
    sh = "engine/shaders/spart_vert.glsl"
    size = None
    if not (root / sh).is_file():
        fail("%s: missing -- the #340 P3b point-size pin has nothing to read" % sh)
    else:
        src = strip_c_comments((root / sh).read_text())
        writes = re.findall(r"\bgl_PointSize\b", src)
        m = re.search(r"#\s*(?:ifdef\s+VULKAN|if\s+defined\s*(?:\(\s*VULKAN\s*\)|\s+VULKAN))[ \t]*\n"
                      r"\s*gl_PointSize\s*=\s*([0-9.]+)[fF]?\s*;\s*\n\s*#\s*endif\b"
                      r"\s*VK_FLIP_Y\s*\(\s*\)\s*\}\s*$", src)
        opened = 1
        if m:
            head = src[:m.start()]
            opened = len(re.findall(r"^\s*#\s*if(?:def|ndef)?\b", head, re.M)) - \
                len(re.findall(r"^\s*#\s*endif\b", head, re.M))
        if len(writes) != 1 or m is None or opened != 0 or num(m.group(1)) is None:
            fail("%s: main() must end `#ifdef VULKAN / gl_PointSize = <size>; / #endif / VK_FLIP_Y()`, "
                 "the file's one gl_PointSize, under no other conditional -- SDL_GPU has no point-size "
                 "state, so a point-list pipeline takes it from the vertex stage (and Vulkan requires the "
                 "write), while GL keeps glPointSize and its preprocessed program (#340 P3b)" % sh)
        else:
            size = num(m.group(1))

    # Every size GL_SetPointSize is handed, outside its own definition.
    sizes = {}
    for p in sorted((root / "engine").rglob("*.c")):
        if "rust" in p.parts:
            continue
        rel = str(p.relative_to(root))
        code = strip_c_comments(p.read_text(errors="replace"))
        for m in POINT_SIZE.finditer(code):
            if re.match(r"float\s+\w+$", m.group(1)):
                continue        # the definition, gl_pipeline.c
            v = num(m.group(1))
            if v is None:
                fail("%s:%d: GL_SetPointSize (%s) -- not a literal, so the Vulkan stage's size cannot be "
                     "held to it; a size that varies needs a uniform in spart_vert.glsl (#340 P3b)"
                     % (rel, code.count("\n", 0, m.start()) + 1, m.group(1)))
            elif v != 1.0:
                sizes.setdefault(v, set()).add(rel)
    if len(sizes) != 1:
        fail("GL_SetPointSize: %d point sizes besides the 1.0 reset (%s) -- spart_vert.glsl writes one "
             "literal under VULKAN, right only while every point batch uses one size; more than one "
             "needs a uniform (#340 P3b)" % (len(sizes), sorted(sizes) or "none"))
    elif size is not None and next(iter(sizes)) != size:
        v = next(iter(sizes))
        fail("%s: gl_PointSize = %g under VULKAN, but GL_SetPointSize sets %g (%s) -- -gpu would draw "
             "square particles at a size GL does not (#340 P3b)" % (sh, size, v, ", ".join(sorted(sizes[v]))))

    # Every point draw is the square-particle batch, on a spart_vert program.
    rp = "engine/hexen2/r_part.c"
    for p in sorted((root / "engine").rglob("*.c")):
        if "rust" in p.parts or re.match(r"rg_\w+\.c$", p.name):
            continue
        rel = str(p.relative_to(root))
        code = strip_c_comments(p.read_text(errors="replace"))
        for n, b, e in functions(code):
            if re.search(r"\bGL_POINTS\b", code[b:e]) and (rel, n) != (rp, "R_DrawParticles_Real"):
                fail("%s: %s draws GL_POINTS -- under -gpu a point's size comes from the vertex stage, "
                     "and only spart_vert.glsl writes one (R_DrawParticles_Real's batch, #340 P3b)"
                     % (rel, n))
    b = body_named(root, rp, "R_DrawParticles_Real")
    if b is not None:
        pts = re.findall(r"\bGL_ImmEnd\s*\(\s*GL_POINTS\s*,\s*(\w+)\s*\)", b)
        sq = re.search(r"\bsquare\s*=\s*!\s*gl_particles\.integer\s*&&\s*!\s*showtris\s*;", b)
        pr = re.search(r"\bprog\s*=\s*showtris\s*\?\s*&gl_shader_flat\s*:\s*OIT_InPass\s*\(\s*\)\s*\?\s*"
                       r"&gl_shader_particle_oit\s*:\s*&gl_shader_particle\s*;", b)
        if not pts or set(pts) != {"prog"} or not sq or not pr or \
           len(re.findall(r"\bGL_POINTS\b", b)) != len(pts):
            fail("%s: R_DrawParticles_Real's points must be GL_ImmEnd (GL_POINTS, prog) alone, with "
                 "`square = !gl_particles.integer && !showtris;` and prog gl_shader_particle or its OIT "
                 "twin outside showtris -- the programs whose vertex stage writes gl_PointSize "
                 "(#340 P3b)" % rp)
    gs = strip_c_comments((root / "engine/h2shared/gl_shader.c").read_text(errors="replace"))
    for prog in ("RPROG_PARTICLE", "RPROG_PARTICLE_OIT"):
        if not re.search(r"\[\s*%s\s*\]\s*=\s*\{[^{}]*\{\s*\{\s*GLSL_VERT_HEADER\s*,\s*spart_vert\s*," % prog, gs):
            fail("engine/h2shared/gl_shader.c: %s's vertex stage must be spart_vert, the one that writes "
                 "gl_PointSize for -gpu's square particles (#340 P3b)" % prog)

    # The fixtures' gate, noclip's.
    cm = "engine/hexen2/cl_main.c"
    g = body_named(root, cm, "CL_TestFixtureAllowed")
    if g is not None:
        cond = re.search(r"\bif\s*\((.*?)\)\s*\{", g, re.S)
        terms = {re.sub(r"\s+", "", t) for t in cond.group(1).split("||")} if cond else set()
        want = {"cls.state!=ca_connected", "!cl.worldmodel", "!sv.active", "*sv_globals.deathmatch",
                "*sv_globals.coop", "skill.integer>2"}
        if terms != want or not re.search(r"return\s+false\s*;\s*\}\s*return\s+true\s*;\s*\}?$", g.strip()):
            fail("%s: CL_TestFixtureAllowed must refuse unless this process serves the map, with no "
                 "deathmatch or coop and skill 2 or under -- noclip's conditions plus a local server "
                 "(`%s`) -- and allow nothing else: testparticles and testentity are cheats (#340 P3b)"
                 % (cm, " || ".join(sorted(want))))
    for rel, name, first in ((rp, "R_TestParticles_f", r"\bAllocParticle\s*\(|\bAngleVectors\s*\("),
                             (cm, "CL_TestEntity_f",
                              r"\bcl_static_entities\b|\bMod_ForName\s*\(|\bcl\.num_statics\b|"
                              r"\bFS_OpenFileHandle\s*\(")):
        b = body_named(root, rel, name)
        if b is None:
            continue
        gate = re.search(r"\bif\s*\(\s*!\s*CL_TestFixtureAllowed\s*\(\s*\)\s*\)\s*return\s*;", b)
        f = re.search(first, b)
        head = b[:gate.start()] if gate else ""
        # at the body's own level, and not inside a conditional it opened (#if 0)
        opened = len(re.findall(r"^\s*#\s*if(?:def|ndef)?\b", head, re.M)) - \
            len(re.findall(r"^\s*#\s*endif\b", head, re.M))
        if not gate or not f or gate.start() > f.start() or \
           head.count("{") - head.count("}") != 1 or opened != 0:
            fail("%s: %s must return on `!CL_TestFixtureAllowed ()` at the body's own level before it "
                 "touches anything -- the gate is what makes it a cheat (#340 P3b)" % (rel, name))
    for rel, cmd, name in ((rp, "testparticles", "R_TestParticles_f"), (cm, "testentity", "CL_TestEntity_f")):
        code = strip_c_comments((root / rel).read_text(errors="replace"))
        if not re.search(r'\bCmd_AddCommand\s*\(\s*"%s"\s*,\s*%s\s*\)' % (cmd, name), code):
            fail('%s: Cmd_AddCommand ("%s", %s) -- tools/sweep-particles-gpu.steps runs it (#340 P3b)'
                 % (rel, cmd, name))


# Every fog read in a shader, the eye distance the stages hand on included.
FOG_READ = re.compile(r"\b(?:u_fog_density|u_fog_color|u_skyfog|u_ffogv|u_fskyfog|v_fogdist)\b")
# What tells the two compiles apart: the macros only the SPIR-V compile
# defines (glslang predefines both), and the version (the GL tier header is
# 430, the Vulkan copy 450).
BACKEND_MACRO = re.compile(r"\b(?:VULKAN|GL_SPIRV|__VERSION__)\b")
# The fogged programs' stages.  Inside a backend conditional these may hold
# directives only (the gl_VertexID aliases), plus the one statement allowed
# below: anything else there could change what fog does on one backend
# without naming fog -- a write to a local after the blend, say.
FOG_STAGES = ("sworld_vert.glsl", "sworld_frag.glsl", "sworld_opaque_frag.glsl",
              "salias_vert.glsl", "salias_inst_vert.glsl", "sskeletal_vert.glsl", "salias_frag.glsl",
              "spart_vert.glsl", "spart_gpu_vert.glsl", "spart_frag.glsl",
              "ssky_layers_vert.glsl", "ssky_layers_frag.glsl")
FOG_STAGE_OK = {("spart_vert.glsl", "gl_PointSize = 2.0;")}     # check_points' write
# The stages that hand v_fogdist on, one location per family: GL links
# varyings by name, Vulkan by location, so a mismatch is invisible on GL and
# leaves -gpu's fog reading another varying.
FOG_VARYING_FAMILIES = (
    ("sworld_vert.glsl", "sworld_frag.glsl", "sworld_opaque_frag.glsl"),
    ("salias_vert.glsl", "salias_inst_vert.glsl", "sskeletal_vert.glsl", "salias_frag.glsl"),
    ("spart_vert.glsl", "spart_gpu_vert.glsl", "spart_frag.glsl"),
)
# Each fogged program's blend, which must stay: (file, the blend).
FOG_BLENDS = (
    ("sworld_frag.glsl", r"\bcolor\.rgb\s*=\s*mix\s*\(\s*u_fog_color\s*,\s*color\.rgb\s*,"),
    ("sworld_opaque_frag.glsl", r"\bcolor\.rgb\s*=\s*mix\s*\(\s*u_fog_color\s*,\s*color\.rgb\s*,"),
    ("salias_frag.glsl", r"\bcolor\.rgb\s*=\s*mix\s*\(\s*u_fog_color\s*,\s*color\.rgb\s*,"),
    ("spart_frag.glsl", r"\bcolor\.rgb\s*=\s*mix\s*\(\s*u_fog_color\s*,\s*color\.rgb\s*,"),
    ("ssky_layers_frag.glsl", r"\bcolor\s*=\s*mix\s*\(\s*color\s*,\s*u_skyfog\.rgb\s*,\s*u_skyfog\.a\s*\)"),
)
# A C function that touches fog in any of these ways never names the backend.
FOG_C_SYMBOL = re.compile(r"\b(?:Fog_\w+|r_skyfog|skyfog|fogdensity|r_fog_density|r_fog_color|R_SetFog|"
                          r"R_SetSkyFog|R_SetWorldFrag|Sky_LayerFogAlpha|Sky_LayerFogColor|Sky_FastColor)\b")
BACKEND_C = re.compile(r"\bR_GPU\b|\brg_active\b|\bUSE_SDL_GPU\b")


def pp_backend_spans(code, rx):
    """[(start, end)] of the text inside a preprocessor conditional whose
    condition (or an #elif of it) matches rx -- the #else arms included."""
    spans, stack, pos = [], [], 0
    for line in code.splitlines(True):
        d = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
        if d:
            k, rest = d.group(1), d.group(2)
            if k in ("ifdef", "ifndef", "if"):
                stack.append([bool(rx.search(rest)), pos + len(line)])
            elif k == "elif" and stack:
                stack[-1][0] = stack[-1][0] or bool(rx.search(rest))
            elif k == "endif" and stack:
                hit, start = stack.pop()
                if hit:
                    spans.append((start, pos))
        pos += len(line)
    return spans


def check_fog(root):
    """#340 P3c: fog is the same uniforms on both backends -- no shader stage
    fogs under one compile only, and nothing on the C side decides fog by
    backend."""
    sd = root / "engine/shaders"
    for p in sorted(sd.glob("*")):
        if p.suffix not in (".glsl", ".inc"):
            continue
        stack = []
        for ln, line in enumerate(strip_c_comments(p.read_text(errors="replace")).splitlines(), 1):
            d = re.match(r"\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", line)
            if d:
                k, rest = d.group(1), d.group(2)
                if k in ("ifdef", "ifndef", "if"):
                    stack.append(bool(BACKEND_MACRO.search(rest)))
                elif k == "elif" and stack:
                    stack[-1] = stack[-1] or bool(BACKEND_MACRO.search(rest))
                elif k == "endif" and stack:
                    stack.pop()
                continue
            if not any(stack) or not line.strip() or re.match(r"\s*#", line):
                continue
            if FOG_READ.search(line):
                fail("engine/shaders/%s:%d: a fog read inside a VULKAN / GL_SPIRV / __VERSION__ conditional "
                     "-- fog is FragParams' u_ffogv / u_fskyfog on both backends, and a stage that fogs under "
                     "one compile only draws fog on GL and not under -gpu, or the reverse (#340 P3c)"
                     % (p.name, ln))
            elif p.name in FOG_STAGES and (p.name, " ".join(line.split())) not in FOG_STAGE_OK:
                fail("engine/shaders/%s:%d: a statement inside a VULKAN / GL_SPIRV / __VERSION__ conditional "
                     "in a fogged program's stage (`%s`) -- it can change the fog one backend draws without "
                     "naming fog; directives only there (#340 P3c)" % (p.name, ln, " ".join(line.split())[:60]))
    for fam in FOG_VARYING_FAMILIES:
        locs = {}
        for f in fam:
            src = strip_c_comments((sd / f).read_text(errors="replace")) if (sd / f).is_file() else ""
            m = re.findall(r"\b(?:VERT_OUT|FRAG_IN)\s*\(\s*(\d+)\s*\)\s*(?:out|in)\s+(?:\w+\s+)?float\s+v_fogdist\s*;", src)
            locs[f] = m[0] if len(m) == 1 else None
        if None in locs.values() or len(set(locs.values())) != 1:
            fail("engine/shaders: v_fogdist must be declared once per stage, at one location, across %s (%s) "
                 "-- Vulkan links varyings by location, so a mismatch leaves -gpu's fog reading another "
                 "varying while GL, linking by name, looks right (#340 P3c)"
                 % (", ".join(fam), ", ".join("%s=%s" % (f, locs[f]) for f in fam)))
    for f, rx in FOG_BLENDS:
        if not (sd / f).is_file():
            fail("engine/shaders/%s: missing -- the #340 P3c fog pin has nothing to read" % f)
        elif not re.search(rx, strip_c_comments((sd / f).read_text(errors="replace"))):
            fail("engine/shaders/%s: its fog blend (`%s`) is gone -- tools/sweep-fog-gpu.steps holds "
                 "this program's fog against GL (#340 P3c)" % (f, re.sub(r"\\[bs]\*?", " ", rx).replace("\\", "").strip()))
    un = sd / "uniforms.inc"
    if un.is_file():
        src = strip_c_comments(un.read_text(errors="replace"))
        blk = re.search(r"FRAG_UBO\s*\(\s*0\s*\)\s*uniform\s+FragParams\s*\{([^}]*)\}", src)
        defs = all(re.search(r"#\s*define\s+%s\s+%s\s*$" % (a, re.escape(b)), src, re.M)
                   for a, b in (("u_fog_color", "u_ffogv.xyz"), ("u_fog_density", "u_ffogv.w"),
                                ("u_skyfog", "u_fskyfog")))
        if not blk or not re.search(r"\bvec4\s+u_ffogv\s*;", blk.group(1)) or \
           not re.search(r"\bvec4\s+u_fskyfog\s*;", blk.group(1)) or not defs:
            fail("engine/shaders/uniforms.inc: u_ffogv and u_fskyfog must be FragParams members, with "
                 "u_fog_color / u_fog_density / u_skyfog their #defines -- the block R_FlushUniforms "
                 "hands SDL_GPU, and the only route fog has to a -gpu draw (#340 P3c)")
    else:
        fail("engine/shaders/uniforms.inc: missing -- the #340 P3c fog pin has nothing to read")

    gu = "engine/h2shared/gl_uniforms.c"
    for name, member in (("R_SetFog", "fog"), ("R_SetSkyFog", "skyfog")):
        b = body_named(root, gu, name)
        if b is not None and (BACKEND_C.search(b) or "return" in b or
                              not re.search(r"\bRU_Set4\s*\(\s*fragparams\.%s\s*," % member, b) or
                              not re.search(r"\bfrag_dirty\s*=\s*true\s*;", b)):
            fail("%s: %s must write fragparams.%s and mark FragParams dirty on both backends, with no "
                 "backend test and no early return (#340 P3c)" % (gu, name, member))
    b = body_named(root, gu, "RU_FlushToBackend")
    if b is not None and not re.search(r"\{\s*&\s*frag_dirty\s*,\s*RU_NAME_FRAG\s*\}", b):
        fail("%s: RU_FlushToBackend's rows must hand FragParams over (`{ &frag_dirty, RU_NAME_FRAG }`) "
             "-- the fog and sky-fog uniforms under -gpu (#340 P3c)" % gu)
    if (root / gu).is_file() and not re.search(
            r"\bRG_UniformBlockRegister\s*\(\s*RU_NAME_FRAG\s*,\s*&\s*fragparams\s*,\s*sizeof\s*\(\s*fragparams\s*\)\s*\)",
            strip_c_comments((root / gu).read_text(errors="replace"))):
        fail("%s: FragParams must be registered with the backend whole "
             "(`RG_UniformBlockRegister (RU_NAME_FRAG, &fragparams, sizeof(fragparams))`) -- unregistered, "
             "RG_UniformBlockFlushed drops its flushes and -gpu draws with no fog (#340 P3c)" % gu)
    b = body_named(root, "engine/h2shared/rg_frame.c", "RG_UniformBlockFlushed")
    if b is not None and not re.search(r"\bmemcpy\s*\(\s*rgb_blocks\s*\[\s*i\s*\]\s*\.\s*flushed\s*,\s*"
                                       r"rgb_blocks\s*\[\s*i\s*\]\s*\.\s*mirror\s*,\s*"
                                       r"rgb_blocks\s*\[\s*i\s*\]\s*\.\s*size\s*\)", b):
        fail("engine/h2shared/rg_frame.c: RG_UniformBlockFlushed must snapshot the whole registered mirror "
             "(`memcpy (rgb_blocks[i].flushed, rgb_blocks[i].mirror, rgb_blocks[i].size)`) -- a partial copy "
             "leaves FragParams' fog members stale under -gpu (#340 P3c)")
    gf = "engine/h2shared/gl_fog.c"
    if (root / gf).is_file():
        code = strip_c_comments((root / gf).read_text(errors="replace"))
        if BACKEND_C.search(code):
            fail("%s: names the backend -- fog is computed once for both, and P3c's sweep holds -gpu "
                 "to GL's fog (#340 P3c)" % gf)
    else:
        fail("%s: missing -- the #340 P3c fog pin has nothing to read" % gf)
    # The fog decisions made on the CPU (the skybox's and the two-pass sky's
    # flat fog passes, r_fastsky's colour, the glows' attenuation, the
    # additive black) never ask which backend draws them.
    for p in sorted((root / "engine").rglob("*.c")):
        if "rust" in p.parts or re.match(r"rg_\w+\.c$", p.name):
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        spans = pp_backend_spans(code, BACKEND_C)
        for n, b, e in functions(code):
            body = code[b:e]
            if not FOG_C_SYMBOL.search(body):
                continue
            ln = code.count("\n", 0, b) + 1
            if BACKEND_C.search(body):
                fail("%s:%d: %s touches fog and names the backend (`%s`) -- fog draws the same under -gpu as "
                     "on GL; a backend decision belongs outside the functions that compute or draw fog "
                     "(#340 P3c)" % (p.relative_to(root), ln, n, BACKEND_C.search(body).group(0)))
            elif any(s0 <= b < s1 for s0, s1 in spans):
                fail("%s:%d: %s touches fog and is compiled under a backend conditional -- fog draws the "
                     "same under -gpu as on GL (#340 P3c)" % (p.relative_to(root), ln, n))


def and_terms(expr):
    """The top-level && operands of a C expression, outer parentheses
    peeled, each with its whitespace collapsed."""
    expr = expr.strip()
    while expr.startswith("(") and expr.endswith(")"):
        depth = 0
        for i, c in enumerate(expr):
            depth += {"(": 1, ")": -1}.get(c, 0)
            if depth == 0 and i < len(expr) - 1:
                break
        else:
            expr = expr[1:-1].strip()
            continue
        break
    out, depth, cur, i = [], 0, "", 0
    while i < len(expr):
        c = expr[i]
        depth += {"(": 1, ")": -1}.get(c, 0)
        if depth == 0 and expr.startswith("&&", i):
            out.append(" ".join(cur.split()))
            cur, i = "", i + 2
            continue
        cur += c
        i += 1
    out.append(" ".join(cur.split()))
    return out


def mirror_cull_term(t):
    """`!(mirrortexturenum >= 0 && r_mirroralpha.value != 1.0)`, its two
    terms in either order, either side of each comparison."""
    m = re.fullmatch(r"!\s*(\(.*\))", t)
    if not m:
        return False
    inner = and_terms(m.group(1))
    if len(inner) != 2:
        return False
    one = r"1(?:\.0*)?f?"
    pats = (r"mirrortexturenum\s*>=\s*0|0\s*<=\s*mirrortexturenum",
            r"r_mirroralpha\s*\.\s*value\s*!=\s*%s|%s\s*!=\s*r_mirroralpha\s*\.\s*value" % (one, one))
    return sorted(i for t2 in inner for i, rx in enumerate(pats) if re.fullmatch(rx, t2)) == [0, 1]


MIRROR_VIEW = (("r_refdef.vieworg", r"r_refdef\s*\.\s*vieworg"), ("r_refdef.viewangles", r"r_refdef\s*\.\s*viewangles"),
               ("r_origin", r"r_origin"), ("vpn", r"vpn"), ("vright", r"vright"), ("vup", r"vup"))


def check_mirror_body(rm, b):
    """R_Mirror's order: the view saved, reflected, drawn, put back after the
    last reflected draw, then the mirror's own surfaces at r_mirroralpha."""
    reflect = re.search(r"\bVectorMA\s*\(\s*r_refdef\s*\.\s*vieworg\s*,", b)
    scene = [m.end() for m in re.finditer(r"\bR_RenderScene\s*\(\s*\)\s*;", b)]
    loops = list(re.finditer(r"\bR_RenderBrushPoly\s*\(\s*&\s*r_worldentity\s*,\s*s\s*,\s*true\s*\)\s*;", b))
    loop = loops[0] if len(loops) == 1 else None
    # The reflected particles, translucent entities and water: the view must
    # stay reflected until the last of them.
    refl = [m.end() for m in re.finditer(r"\b(?:R_DrawParticles|R_DrawTransEntitiesOnList|R_DrawWaterSurfaces)"
                                         r"\s*\([^;]*\)\s*;", b)]
    ok = reflect is not None and len(scene) == 1 and loop is not None and bool(refl) and \
        all(scene[0] <= e <= loop.start() for e in refl)
    last = max(refl) if refl else 0
    missing = []
    if ok:
        pairs = [(n, r"\bVectorCopy\s*\(\s*%s\s*,\s*(\w+)\s*\)\s*;" % rx,
                  r"\bVectorCopy\s*\(\s*{0}\s*,\s*%s\s*\)\s*;" % rx) for n, rx in MIRROR_VIEW]
        pairs += [("r_viewleaf", r"\b(\w+)\s*=\s*r_viewleaf\s*;", r"\br_viewleaf\s*=\s*{0}\s*;"),
                  ("cl.cshifts[CSHIFT_CONTENTS]", r"\b(\w+)\s*=\s*cl\s*\.\s*cshifts\s*\[\s*CSHIFT_CONTENTS\s*\]\s*;",
                   r"\bcl\s*\.\s*cshifts\s*\[\s*CSHIFT_CONTENTS\s*\]\s*=\s*{0}\s*;"),
                  ("v_blend", r"\b(?:memcpy\s*\(\s*(\w+)\s*,\s*v_blend\s*,[^;]*|Vector4Copy\s*\(\s*v_blend\s*,\s*(\w+)\s*)\)\s*;",
                   r"\b(?:memcpy\s*\(\s*v_blend\s*,\s*{0}\s*,[^;]*|Vector4Copy\s*\(\s*{0}\s*,\s*v_blend\s*)\)\s*;")]
        for n, save_rx, back_rx in pairs:
            sv = re.search(save_rx, b)
            name = sv and next(g for g in sv.groups() if g)
            back = re.search(back_rx.replace("{0}", re.escape(name)), b) if sv else None
            if not (sv and back and sv.end() <= reflect.start() and last <= back.start()
                    and back.end() <= loop.start()):
                missing.append(n)
    if not ok or missing:
        fail("%s: R_Mirror must copy the view aside before reflecting it -- r_refdef.vieworg and .viewangles, and "
             "what R_SetupFrame derives from them: r_origin, vpn, vright, vup, r_viewleaf, the contents colour "
             "shift and v_blend -- and copy each back after the last reflected draw (particles, translucent "
             "entities, water), ahead of the mirror's own surfaces%s.  While paused V_RenderView does not "
             "recompute r_refdef, and every other frame rendered the reflection full screen; the rest feed the "
             "listener, the motion blur, the water warp, R_PolyBlend and the next R_MarkLeaves (#340 P3d)"
             % (rm, " (wrong: %s)" % ", ".join(missing) if missing else ""))
    blend = [m.end() for m in re.finditer(r"\bR_SetBlend\s*\(\s*true\s*\)\s*;", b)]
    good = loop is not None and bool(blend) and blend[-1] <= loop.start()
    if good:
        # Between turning blending on and the loop, exactly these, once each,
        # and nothing else that sets blend, colour or the alpha override.
        seg = b[blend[-1]:loop.start()]
        want = (r"\bR_SetBlendFunc\s*\(\s*GL_SRC_ALPHA\s*,\s*GL_ONE_MINUS_SRC_ALPHA\s*\)\s*;",
                r"\bGL_ImmColor4f\s*\(\s*1(?:\.0*)?f?\s*,\s*1(?:\.0*)?f?\s*,\s*1(?:\.0*)?f?\s*,\s*"
                r"r_mirroralpha\s*\.\s*value\s*\)\s*;",
                r"\bGL_SetForceOpaqueAlpha\s*\(\s*0(?:\.0*)?f?\s*\)\s*;")
        good = all(len(re.findall(rx, seg)) == 1 for rx in want) and \
            len(re.findall(r"\b(?:R_SetBlend|R_SetBlendFunc|GL_ImmColor4f|GL_SetForceOpaqueAlpha)\s*\(", seg)) == 3
        # The colour put back after the loop, the next colour set.
        tail = b[loop.end():]
        reset = re.search(r"\bGL_ImmColor4f\s*\(\s*1(?:\.0*)?f?\s*,\s*1(?:\.0*)?f?\s*,\s*1(?:\.0*)?f?\s*,\s*"
                          r"1(?:\.0*)?f?\s*\)\s*;", tail)
        good = good and reset is not None and not re.search(r"\bGL_ImmColor4f\s*\(", tail[:reset.start()])
    if not good:
        fail("%s: R_Mirror must turn blending on and then set, once each and nothing else between them and "
             "the loop, R_SetBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA), GL_ImmColor4f (1, 1, 1, "
             "r_mirroralpha.value) and GL_SetForceOpaqueAlpha (0.0f), draw the mirror's surfaces with "
             "R_RenderBrushPoly (&r_worldentity, s, true), and put the colour back to (1, 1, 1, 1) after -- the "
             "alpha the modernization dropped, without which the mirror's texture covers the reflection whole; "
             "the override reset holds it there whatever the reflection drew last (#340 P3d)" % rm)


MIRROR_FUNCS = (("engine/hexen2/gl_rmain.c", "R_Mirror"), ("engine/hexen2/gl_rmain.c", "R_Clear"),
                ("engine/hexen2/gl_rmain.c", "R_SetupGL"), ("engine/hexen2/gl_rsurf.c", "R_MirrorChain"),
                ("engine/hexen2/gl_rsurf.c", "R_RenderBrushPoly"))


def check_mirror(root):
    """#340 P3d: the mirror draws the same on both backends -- the path
    names no backend, the depth range and cull face it switches reach
    SDL_GPU, and the GL fixes the sweep needed stay in."""
    for rel, name in MIRROR_FUNCS:
        if not (root / rel).is_file():
            fail("%s: missing -- the #340 P3d mirror pin has nothing to read" % rel)
            continue
        code = strip_c_comments((root / rel).read_text(errors="replace"))
        spans = pp_backend_spans(code, BACKEND_C)
        found = [(b, e) for n, b, e in functions(code) if n == name]
        if len(found) != 1:
            fail("%s: %d definitions of %s, the #340 P3d mirror pin reads exactly one" % (rel, len(found), name))
            continue
        b, e = found[0]
        if BACKEND_C.search(code[b:e]) or any(s0 <= b < s1 for s0, s1 in spans):
            fail("%s: %s names the backend or is compiled under a backend conditional -- the mirror "
                 "(the reflected view, the depth-range split, the cull flip, the blend over it) draws the "
                 "same under -gpu as on GL, and tools/sweep-mirror-gpu.steps holds it there (#340 P3d)"
                 % (rel, name))

    rm = "engine/hexen2/gl_rmain.c"
    b = body_named(root, rm, "R_Mirror")
    if b is not None:
        check_mirror_body(rm, b)
    rs = "engine/hexen2/gl_rsurf.c"
    b = body_named(root, rs, "R_RenderBrushPoly")
    if b is not None:
        # The colour, under `if (!override)`, bare or braced.
        col = r"GL_ImmColor4f\s*\(\s*intensity\s*,\s*intensity\s*,\s*intensity\s*,\s*alpha_val\s*\)\s*;"
        guarded = list(re.finditer(r"\bif\s*\(\s*!\s*override\s*\)\s*(?:\{\s*%s\s*\}|%s)" % (col, col), b))
        # After the guard, a mirror surface (a plain wall: not sky, not turb)
        # must meet no other colour; the turb branch, which sets its own, is
        # the one block a mirror never enters.
        rest = b[guarded[0].end():] if len(guarded) == 1 else ""
        for m in list(re.finditer(r"\bif\s*\(\s*fa->flags\s*&\s*SURF_DRAWTURB\s*\)\s*\{", rest))[::-1]:
            depth, i = 0, m.end() - 1
            while i < len(rest):
                depth += {"{": 1, "}": -1}.get(rest[i], 0)
                if depth == 0:
                    break
                i += 1
            rest = rest[:m.start()] + rest[i + 1:]
        if len(guarded) != 1 or "EmitBothSkyLayers" not in rest or re.search(r"\bGL_ImmColor4f\s*\(", rest):
            fail("%s: R_RenderBrushPoly's colour (`GL_ImmColor4f (intensity, intensity, intensity, "
                 "alpha_val)`) must be set under `if (!override)` once, ahead of the sky, with no other colour "
                 "after it outside the turb branch -- override is R_Mirror keeping r_mirroralpha (#340 P3d)" % rs)
    b = body_named(root, rs, "R_DrawWorld")
    if b is not None:
        m = re.search(r"\bgpu_cull_active\s*=\s*([^;]*);", b)
        if not m or not any(mirror_cull_term(t) for t in and_terms(m.group(1))):
            fail("%s: R_DrawWorld's GL compute cull must be off on a map with a mirror while mirrors are on "
                 "(`&& !(mirrortexturenum >= 0 && r_mirroralpha.value != 1.0)`) -- its filter draws the "
                 "mirror's texture opaque over the reflection, which -gpu (no compute) never does, so GL "
                 "and -gpu part on every mirror frame (#340 P3d)" % rs)
    # override = true is R_Mirror's alone: any other caller would draw with
    # whatever colour was left over.
    for p in sorted((root / "engine").rglob("*.c")):
        if "rust" in p.parts:
            continue
        code = strip_c_comments(p.read_text(errors="replace"))
        for m in re.finditer(r"\bR_RenderBrushPoly\s*\(([^;]*)\)\s*;", code):
            last = m.group(1).rsplit(",", 1)[-1].strip()
            # A prototype names the parameter; false, FALSE and 0 all leave the colour to it.
            if last not in ("false", "FALSE", "0") and not re.search(r"\bqboolean\b", m.group(1)) and \
               not (p.name == "gl_rmain.c" and any(n == "R_Mirror" and b0 <= m.start() < e0
                                                    for n, b0, e0 in functions(code))):
                fail("%s:%d: R_RenderBrushPoly (%s) passes override outside R_Mirror -- override keeps the "
                     "caller's colour, which only R_Mirror sets (#340 P3d)"
                     % (p.relative_to(root), code.count("\n", 0, m.start()) + 1, m.group(1).strip()))

    # The mirror is where the player's own model is first seen: its skin is a
    # texture made at init and filled after a map has grown the name table.
    for rel, fn, var, tail in (("engine/h2shared/rg_resource.c", "RGT_QueueUpload", "t", "uploads"),
                               ("engine/h2shared/rg_buffer.c", "RGB_Queue", "b", "uploads")):
        b = body_named(root, rel, fn)
        if b is None:
            continue
        rederive = re.search(r"\bif\s*\(\s*!\s*%s->%s\s*\)\s*%s->%s_tail\s*=\s*&\s*%s->%s\s*;"
                             % (var, tail, var, tail, var, tail), b)
        append = re.search(r"\*\s*%s->%s_tail\s*=\s*u\s*;" % (var, tail), b)
        if not rederive or not append or rederive.end() > append.start():
            fail("%s: %s must re-derive an empty list's tail (`if (!%s->%s) %s->%s_tail = &%s->%s;`) before "
                 "appending -- the tail of an empty list points into the name table, which growing it "
                 "moves, and the upload was written into the freed array and lost (#340 P3d)"
                 % (rel, fn, var, tail, var, tail, var, tail))

    # What the mirror switches mid-frame, and how it reaches SDL_GPU.
    rf = "engine/h2shared/rg_frame.c"
    b = body_named(root, rf, "RGF_CommitDraw")
    if b is not None and not (re.search(r"\bd->vp\.min_depth\s*=\s*\(float\)\s*ps->depth_near\s*;", b) and
                              re.search(r"\bd->vp\.max_depth\s*=\s*\(float\)\s*ps->depth_far\s*;", b)):
        fail("%s: RGF_CommitDraw must give each draw's viewport the shadow's depth range, "
             "`d->vp.min_depth = (float) ps->depth_near;` and `d->vp.max_depth = (float) ps->depth_far;` -- "
             "the mirror draws the reflection into one half of it and the view into the other (#340 P3d)" % rf)
    rp = "engine/h2shared/rg_pipeline.c"
    if (root / rp).is_file():
        pipe = strip_c_comments((root / rp).read_text(errors="replace"))
        if not re.search(r"\bif\s*\(\s*s->cull\s*\)\s*\{\s*if\s*\(\s*s->cull_face\s*==\s*GL_FRONT_AND_BACK\s*\)\s*"
                         r"return\s+false\s*;\s*k->cull_face\s*=\s*\(\s*s->cull_face\s*==\s*GL_FRONT\s*\)\s*\?\s*"
                         r"GL_FRONT\s*:\s*GL_BACK\s*;\s*\}", pipe) or \
           not re.search(r"\bcull_mode\s*=\s*\(\s*k->cull_face\s*==\s*GL_FRONT\s*\)\s*\?\s*SDL_GPU_CULLMODE_FRONT\s*:"
                         r"\s*\(\s*k->cull_face\s*==\s*GL_BACK\s*\)\s*\?\s*SDL_GPU_CULLMODE_BACK\s*:\s*"
                         r"SDL_GPU_CULLMODE_NONE\s*;", pipe):
            fail("%s: the pipeline key must take the shadow's cull face only while culling is on (GL_FRONT "
                 "or GL_BACK, GL_FRONT_AND_BACK refused) and map GL_FRONT / GL_BACK / none to "
                 "SDL_GPU_CULLMODE_FRONT / BACK / NONE -- the mirror's reflected view culls GL_BACK, its "
                 "projection flipped (#340 P3d)" % rp)
    else:
        fail("%s: missing -- the #340 P3d mirror pin has nothing to read" % rp)


# gl_postprocess.c functions that stay raw GL, and why.
PP_RAW_FUNCS = {
    "PP_BuildPaletteLUT_GPU": "the compute LUT builder, off under -gpu through compute_shaders (P5)",
    "PP_FreePaletteGPU": "its program; 0 under -gpu, where the builder never links it (P5)",
}
# (function, GL entry point) -> (calls, why) that stay raw in the rest.
PP_RAW_OK = {
    ("PP_CreateFBO", "glRenderbufferStorageMultisample"):
        (2, "the MSAA branch's storage, P6: -gpu forces 0 samples ahead of it"),
    ("GL_PostProcess_EndFrame", "glDrawArrays"):
        (3, "bloom's bufferless fullscreen triangles: R_DrawArrays is P4e"),
    ("GL_PostProcess_Shutdown", "glDeleteProgram"):
        (3, "programs at shutdown, restart-only under -gpu (P4 decision 9)"),
    ("GL_PostProcess_Shutdown", "glDeleteBuffers"):
        (4, "each under `if (*_ubo)`: -gpu creates none of the four"),
}
# The GL entry point each gl_fbo.c seam is the bare call to.
FBO_SEAMS = ("GenFramebuffers", "DeleteFramebuffers", "BindFramebuffer", "FramebufferTexture2D",
             "FramebufferRenderbuffer", "CheckFramebufferStatus", "DrawBuffers",
             "GenRenderbuffers", "DeleteRenderbuffers", "BindRenderbuffer",
             "RenderbufferStorage", "BlitFramebuffer")


def gl_arm(body):
    """A seam body with its `#ifdef USE_SDL_GPU ... #endif` blanked."""
    return re.sub(r"#\s*ifdef\s+USE_SDL_GPU\b.*?#\s*endif", " ", body, flags=re.S)


def check_fbo_seam(root):
    rel = "engine/h2shared/gl_fbo.c"
    path = root / rel
    if not path.is_file():
        fail("%s: missing -- the #340 P4a-1 FBO seam" % rel)
        return
    code = strip_c_comments(path.read_text(errors="replace"))
    heads = {m.group(1): m.group(2) for m in
             re.finditer(r"^\w[\w \t\*]*?\b(R_\w+)\s*\(([^;{}]*)\)\s*\{", code, re.M)}
    bodies = {n: code[b:e + 1] for n, b, e in functions(code)}
    for name in FBO_SEAMS + ("FramebufferBinding",):
        fn = "R_" + name
        if fn not in bodies or fn not in heads:
            fail("%s: no %s -- gl_fbo.h's seam is gone" % (rel, fn))
            continue
        params = [re.findall(r"\w+", a)[-1] for a in heads[fn].split(",") if a.strip()]
        got = re.sub(r"\s+", "", gl_arm(bodies[fn]))
        if name == "FramebufferBinding":
            want = "{GLintfbo=0;glGetIntegerv_fp(pname,&fbo);return(GLuint)fbo;}"
        else:
            call = "gl%s_fp(%s);" % (name, ",".join(params))
            want = {"CheckFramebufferStatus": "{return%s}" % call,
                    "BlitFramebuffer": "{%sreturntrue;}" % call}.get(name, "{%s}" % call)
        if got != want:
            fail("%s: %s's GL arm must be exactly the bare call, its parameters in order: want `%s`, "
                 "found `%s` (whitespace dropped; #340 P4a-1)" % (rel, fn, want, got))
        arm = re.search(r"#\s*ifdef\s+USE_SDL_GPU\b(.*?)#\s*endif", bodies[fn], re.S)
        arm = re.sub(r"\s+", "", arm.group(1)) if arm else ""
        note = r'RG_NoteUnhandled\("%s\(rendertargetsareP4a-2\)"\);' % fn
        tail = {"GenFramebuffers": r"RG_FboZeroNames\(n,fbos\);return;",
                "GenRenderbuffers": r"RG_FboZeroNames\(n,rbs\);return;",
                "CheckFramebufferStatus": r"returnGL_FRAMEBUFFER_UNSUPPORTED;",
                "BlitFramebuffer": r"returnfalse;",
                "FramebufferBinding": r"return0;"}.get(name, r"return;")
        if not re.fullmatch(r"if\(R_GPU\(\)\)\{%s%s\}" % (note, tail), arm):
            fail("%s: %s's SDL_GPU arm must note the call and answer neutrally until P4a-2 builds "
                 "rg_target.c -- zero names, GL_FRAMEBUFFER_UNSUPPORTED, false, 0 -- found `%s` "
                 "(#340 P4a-1)" % (rel, fn, arm))
    zb = bodies.get("RG_FboZeroNames", "")
    if not re.search(r"for\s*\(\s*i\s*=\s*0\s*;\s*i\s*<\s*n\s*;\s*i\+\+\s*\)\s*names\s*\[\s*i\s*\]\s*=\s*0\s*;", zb):
        fail("%s: RG_FboZeroNames must zero all n names (#340 P4a-1)" % rel)
    cm = (root / "engine/CMakeLists.txt").read_text()
    if not re.search(r"\$\{COMMONDIR\}/gl_texbind\.c\s*\n\s*\$\{COMMONDIR\}/gl_fbo\.c\b", cm):
        fail("engine/CMakeLists.txt: gl_fbo.c must follow gl_texbind.c in the client's source list (#340 P4a-1)")


def check_postprocess(root):
    """#340 P4a-1: the chain names no GL entry point the seams cover."""
    check_fbo_seam(root)
    rel = "engine/h2shared/gl_postprocess.c"
    if not (root / rel).is_file():
        fail("%s: missing -- the #340 P4a-1 pins have nothing to read" % rel)
        return
    code = strip_c_comments((root / rel).read_text(errors="replace"))
    gated = {name for r, name, *_ in GPU_GATES if r == rel}
    seen = {}
    names = set()
    for name, b, e in functions(code):
        if not name:
            continue
        names.add(name)
        if name.startswith("OIT_") or name in PP_RAW_FUNCS:
            continue
        body = code[b:e + 1]
        arms = [] if name in gated else gpu_arms(body)
        for m in re.finditer(r"\b(gl[A-Z]\w*)_fp\s*\(", body):
            if arms and excluded(body, arms, m.start()):
                continue
            key = (name, m.group(1))
            if key in PP_RAW_OK:
                seen[key] = seen.get(key, 0) + 1
                if key[1] == "glDeleteBuffers" and not in_name_guard(body, m.start(), r"\w+_ubo"):
                    fail("%s: %s calls glDeleteBuffers outside an `if (*_ubo)` guard" % (rel, name))
                continue
            ln = code.count("\n", 0, b + m.start()) + 1
            fail("%s:%d: %s calls %s_fp raw -- the post-process chain goes through gl_fbo.h, "
                 "R_SetViewport, R_ProgramSamplerLoc / R_ProgramSamplerUnit and the VAO seams, or an "
                 "SDL_GPU arm excludes the call (#340 P4a-1)" % (rel, ln, name, m.group(1)))
    for f in PP_RAW_FUNCS:
        if f not in names:
            fail("%s: PP_RAW_FUNCS names %s, which is gone -- drop the entry" % (rel, f))
    for key, (n, why) in PP_RAW_OK.items():
        if seen.get(key, 0) != n:
            fail("%s: %s makes %d raw %s calls, PP_RAW_OK expects %d (%s) -- route the new one or fix "
                 "the count" % (rel, key[0], seen.get(key, 0), key[1], n, why))

    def body(name):
        return body_named(root, rel, name)

    gate = r"#\s*ifdef\s+USE_SDL_GPU\s*\n\s*if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*%s\s*#\s*endif"
    b = body("PP_CreateFBO")
    if b is not None:
        g = re.search(gate % r"samples\s*=\s*0\s*;", b)
        clamp = re.search(r"samples\s*=\s*gl_max_samples\s*;", b)
        msaa = re.search(r"\bif\s*\(\s*samples\s*>\s*1\b", b)
        if not (g and clamp and msaa and clamp.end() < g.start() < msaa.start()):
            fail("%s: PP_CreateFBO needs `#ifdef USE_SDL_GPU / if (R_GPU ()) samples = 0; / #endif` "
                 "after the gl_max_samples clamp and before the MSAA branch -- MSAA targets are P6, and "
                 "gl_max_samples being 0 under -gpu is an accident to lean on (#340 P4a-1)" % rel)
        elif b[:g.start()].count("{") - b[:g.start()].count("}") != 1:
            fail("%s: PP_CreateFBO's samples gate must sit at the body's own level" % rel)
    b = body("GL_PostProcess_BeginFrame")
    if b is not None:
        m = re.search(r"\bif\s*\(\s*pp_fbo_failed\s*\)\s*\{", b)
        blk = b[m.end() - 1:stmt_end(b, m.end() - 1)] if m else ""
        on = re.search(r"\bpp_active\s*=\s*true\b", blk)
        # an `if (R_GPU ())` whose body always returns, under #ifdef USE_SDL_GPU,
        # at the block's own level, ahead of the copyback turning the chain on
        g = [(st, tb, te) for st, neg, (tb, te), els in gpu_arms(blk)
             if not neg and returns_always(blk, tb, te)
             and re.search(r"#\s*ifdef\s+USE_SDL_GPU\s*$", blk[:st].rstrip())
             and blk[:st].count("{") - blk[:st].count("}") == 1]
        if not (m and g and on and g[0][2] <= on.start()):
            fail("%s: GL_PostProcess_BeginFrame's `if (pp_fbo_failed)` block needs `#ifdef "
                 "USE_SDL_GPU / if (R_GPU ()) { ... return; } / #endif` at its own level before "
                 "`pp_active = true` -- the copyback reads the framebuffer, which -gpu does not do "
                 "(#340 P4a-1)" % rel)
    b = body("GL_SoftDepth_Capture")
    if b is not None and (not re.search(r"\bsoft_depth_valid\s*=\s*R_BlitFramebuffer\s*\(", b)
                          or re.search(r"\bsoft_depth_valid\s*=\s*true\b", b)):
        fail("%s: GL_SoftDepth_Capture must set soft_depth_valid from R_BlitFramebuffer's result and "
             "nowhere to true -- a blit the -gpu arm declines must not leave sprites sampling a "
             "snapshot nothing wrote (P4b lifts the gate; #340 P4a-1)" % rel)
    decl = re.search(r"\bstatic\s+float\s+pp_vert_mvp\s*\[\s*16\s*\]\s*;", code)
    if not decl:
        fail("%s: pp_vert_mvp is not `static float pp_vert_mvp[16];` -- PpVert's mirror is 64 bytes"
             % rel)
    b = body("PP_InitShader")
    if b is not None:
        reg = [(m.start(), m.group(1), re.sub(r"\s+", "", m.group(2)), re.sub(r"\s+", "", m.group(3)))
               for m in re.finditer(r"\bRG_UniformBlockRegister\s*\(\s*\"(\w+)\"\s*,\s*([^,]+),\s*"
                                    r"sizeof\s*\(([^)]*)\)\s*\)", b)]
        want = {("PpVert", "pp_vert_mvp", "pp_vert_mvp"), ("PpParams", "&pp_params", "pp_params")}
        arms = gpu_arms(b)
        inarm = all(any(not neg and tb <= pos < te for _, neg, (tb, te), _ in arms)
                    for pos, *_ in reg)
        if {r[1:] for r in reg} != want or len(reg) != 2 or not inarm:
            fail("%s: PP_InitShader must register exactly RG_UniformBlockRegister (\"PpVert\", "
                 "pp_vert_mvp, sizeof(pp_vert_mvp)) and (\"PpParams\", &pp_params, sizeof(pp_params)) "
                 "inside its `if (R_GPU ())` arm (#340 P4a-1)" % rel)
    b = body("PP_PushParams")
    if b is not None:
        arm = re.search(r"#\s*ifdef\s+USE_SDL_GPU\s*\n\s*if\s*\(\s*R_GPU\s*\(\s*\)\s*\)\s*\{\s*"
                        r"RG_UniformBlockFlushed\s*\(\s*\"PpVert\"\s*\)\s*;\s*"
                        r"RG_UniformBlockFlushed\s*\(\s*\"PpParams\"\s*\)\s*;\s*return\s*;\s*\}\s*#\s*endif", b)
        mvp = re.search(r"\bGL_GetMVP\s*\(\s*pp_vert_mvp\s*\)", b)
        if not (arm and mvp and mvp.end() <= arm.start()):
            fail("%s: PP_PushParams needs GL_GetMVP (pp_vert_mvp) and then `#ifdef USE_SDL_GPU / if "
                 "(R_GPU ()) { RG_UniformBlockFlushed (\"PpVert\"); RG_UniformBlockFlushed "
                 "(\"PpParams\"); return; } / #endif` -- a flush before the MVP is written pushes last "
                 "draw's matrix (#340 P4a-1)" % rel)
    b = body("PP_BindUniformBlock")
    if b is not None and not gpu_return_before(b, r"\bgl\w+_fp\s*\("):
        fail("%s: PP_BindUniformBlock must return under R_GPU () before any GL call: the program's "
             "reflection fixes the slots, and the null glGetUniformBlockIndex answers 0, so its size "
             "query warned at every -gpu boot (#340 P4a-1)" % rel)
    # RG_MAX_BLOCKS: what is registered, and room for P4e's BloomParams.
    fr = (root / "engine/h2shared/rg_frame.c").read_text(errors="replace")
    mx = re.search(r"#\s*define\s+RG_MAX_BLOCKS\s+(\d+)", fr)
    blocks = set()
    for pth in sorted((root / "engine").rglob("*.c")):
        if "rust" in pth.parts or pth.name.startswith("rg_"):
            continue
        for m in re.finditer(r"\bRG_UniformBlockRegister\s*\(\s*([^,]+?)\s*,",
                             strip_c_comments(pth.read_text(errors="replace"))):
            blocks.add(m.group(1))
    if not mx:
        fail("rg_frame.c: no #define RG_MAX_BLOCKS")
    elif len(blocks) < 9 or len(blocks) + 1 > int(mx.group(1)):
        fail("RG_MAX_BLOCKS is %s and %d distinct blocks are registered (%s): it needs room for P4e's "
             "BloomParams, and fewer than 9 means the scan is broken (#340 P4a-1)"
             % (mx.group(1), len(blocks), ", ".join(sorted(blocks))))


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
    check_stencil(root)
    check_buffers(root)
    check_storage(root)
    check_translucency(root)
    check_points(root)
    check_fog(root)
    check_mirror(root)
    check_postprocess(root)
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
