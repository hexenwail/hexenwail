#!/usr/bin/env python3
"""Vertex, index and storage buffers, vertex-array state and draws go
through the seams (GitHub #340, phases P2d and P2e).

GL keeps vertex input in objects the call names only by binding: a buffer is
filled through whatever is bound to its target, a VAO records whatever
array buffer was bound when each pointer was set, and a draw reads all of it
from the bound VAO.  The SDL_GPU backend (#340) has no such binding.  It
needs every buffer's contents, every attribute's format and source, and
every draw, in its own hands -- so these calls go through
engine/h2shared/gl_buffer.c's seams (GL_BindBuffer, R_GenBuffers,
R_DeleteBuffers, R_BufferData, R_BufferSubData, R_GenVertexArrays,
R_DeleteVertexArrays, R_BindVertexArray, R_Enable/DisableVertexAttribArray,
R_VertexAttribPointer, R_VertexAttribIPointer, R_VertexAttrib4f) and
engine/h2shared/gl_vbo.c's (R_DrawElements, R_DrawElementsInstanced,
R_DrawArrays since P4e, R_DrawElementsIndirect since P5a-1, and
GL_ImmEnd / GL_ImmDraw for the immediate batches).  Each GL arm is the bare
call; each -gpu arm reaches rg_buffer.c / rg_frame.c.  A raw call anywhere
else is a buffer or a draw that backend never hears about: under -gpu it
lands in the null table.

So, outside gl_buffer.c and gl_vbo.c, none of these, whatever the file:
  * buffer names and storage: glGenBuffers, glCreateBuffers,
    glDeleteBuffers, glBufferStorage, glNamedBuffer{Data,SubData,Storage},
    glMapBuffer, glMapBufferRange, glUnmapBuffer, glFlushMappedBufferRange,
    glCopyBufferSubData, glCopyNamedBufferSubData, glClearBuffer{Data,
    SubData}, glInvalidateBuffer{Data,SubData};
  * glBindBuffer, glBufferData and glBufferSubData, unless the first
    argument is a literal target other than GL_ARRAY_BUFFER,
    GL_ELEMENT_ARRAY_BUFFER and GL_SHADER_STORAGE_BUFFER: a uniform block,
    indirect or pixel buffer is another phase's (uniform blocks are
    mirrored by gl_uniforms.c).  Storage counts since P2e, which routes
    the storage buffers a graphics stage reads.  A first argument that is
    not a literal GL_* name counts as a vertex target;
  * indexed buffer bindings, whatever the target: glBindBufferBase,
    glBindBufferRange, glBindBuffersBase, glBindBuffersRange.  A storage
    binding point is what -gpu binds a draw's storage slot to (#340 P2e),
    and gl_buffer.c's GL_BindBufferBase / Range keep a cache of every
    point that a raw bind would leave stale;
  * vertex-array state: glGenVertexArrays, glCreateVertexArrays,
    glDeleteVertexArrays, glBindVertexArray, glVertexAttribPointer,
    glVertexAttribIPointer, glVertexAttribLPointer,
    glEnableVertexAttribArray, glDisableVertexAttribArray, the
    glVertexAttrib{1,2,3,4}{f,fv,d,dv,s,sv} / glVertexAttrib4N* /
    glVertexAttribI4{i,ui}{,v} current values, glVertexAttrib{,I,L}Format,
    glVertexAttribBinding, glVertexAttribDivisor, glBindVertexBuffer(s),
    glVertexBindingDivisor, and the DSA glVertexArray* /
    gl{Enable,Disable}VertexArrayAttrib spellings;
  * draws: glDrawArrays, glDrawElements and every Instanced / BaseVertex /
    BaseInstance / Range / Indirect / MultiDraw variant of them.

Every spelling counts, as in tools/test_texture_create_routing.py, whose
scanner this is: raw, loader table (_fp), extension-suffixed, called
through parentheses or a dereference, and taken as a value.

In gl_buffer.c and gl_vbo.c themselves, only the functions OWNERS names
may make these calls: the seams and the GL-only machinery beside them.

Allowed outside the two seam files:
  * ALLOWED: (file, GL function without gl/_fp/suffix) -> (exact count,
    why).  The ledger of what P2d leaves raw: the paths -gpu does not run
    yet, each with the phase that routes it.  A count that no longer
    matches fails -- a new raw call in a listed file is not absorbed by the
    entry -- and so does an entry matching nothing, so the list cannot
    outlive the code.  The world, brush-model and sky paths
    (engine/hexen2/gl_rsurf.c, gl_rmain.c) have no entry: every call there
    is routed.  Nor, since P2e, do the alias meshes (gl_mesh.c: the MDL /
    MD3 VAOs, the skeletal VAO, the pose and bone storage buffers) or the
    instanced alias programs' ShadeDots buffer (gl_shader.c).  Nor, since
    P5a-1, do the compute passes (gl_worldcull.c's cull buffers, indirect
    draw and world VAO rebind, gl_lightcluster.c's light buffer, gl_ssao.c's
    uniform buffer and composite draw): their -gpu arms are P5's, and the
    passes stay off under -gpu until each sub-phase lifts its gate.
  * engine/h2shared/gl_func.h, the loader table, which declares them;
  * engine/h2shared/rg_null.c, the generated null table, which stubs them;
  * engine/h2shared/web_canvas_gl2.c, the software renderer's WebGL2
    presentation canvas (raw GLES by design, as the texture lint).

Scope: engine/**/*.c, *.h and *.inc.  Comments and string/char literals
are blanked first (line numbers kept).  #if 0 blocks are NOT skipped.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]

# The seam files, and in each the functions that may make the raw calls: the
# seams themselves (tools/test_rg_sources.py holds each to its -gpu arm and
# one bare call) and the GL-only machinery beside them.  Any other function
# in these files is checked like every other file.
OWNERS = {
    "engine/h2shared/gl_buffer.c": {
        "GL_BindBuffer", "R_GenBuffers", "R_DeleteBuffers", "R_BufferData", "R_BufferSubData",
        "GL_BindBufferBase", "GL_BindBufferRange", "GL_BindBuffersRange",
        "R_GenVertexArrays", "R_DeleteVertexArrays", "R_BindVertexArray", "R_EnableVertexAttribArray",
        "R_DisableVertexAttribArray", "R_VertexAttribPointer", "R_VertexAttribIPointer", "R_VertexAttrib4f",
        # the GL 4.3 frame ring and the particle SSBO: GL-only.  -gpu leaves the ring on the null
        # table and never makes the SSBO (r_part.c's R_GPU_Particles_Init returns, #340 P3a)
        "GL_CreateParticleResources", "GL_DeleteParticleResources", "GL_AddGarbageBufferTo",
        "GL_DrainGarbage", "GL_AllocFrameHostBuffers", "GL_DeleteFrameResources", "GL_Upload",
    },
    "engine/h2shared/gl_vbo.c": {
        "R_DrawElements", "R_DrawElementsInstanced", "R_DrawArrays",
        # the GPU world cull's indirect draw (#340 P5a-1; its -gpu arm is P5c)
        "R_DrawElementsIndirect",
        # the immediate emulator's GL arms; test_rg_sources.py holds every GL
        # call in gl_vbo.c to an SDL_GPU arm
        "GL_VBO_Init", "GL_VBO_Shutdown", "GL_ImmBindVerts", "GL_ImmEnd", "GL_ImmDraw",
    },
}
EXEMPT = {
    "engine/h2shared/gl_func.h",
    "engine/h2shared/web_canvas_gl2.c",
    "engine/h2shared/rg_null.c",
}
SUFFIXES = (".c", ".h", ".inc")

BUFFERS = (
    "GenBuffers", "CreateBuffers", "DeleteBuffers", "BufferStorage",
    "NamedBufferData", "NamedBufferSubData", "NamedBufferStorage",
    "MapBuffer", "MapBufferRange", "MapNamedBuffer", "MapNamedBufferRange",
    "UnmapBuffer", "UnmapNamedBuffer", "FlushMappedBufferRange", "FlushMappedNamedBufferRange",
    "CopyBufferSubData", "CopyNamedBufferSubData",
    "ClearBufferData", "ClearBufferSubData", "ClearNamedBufferData", "ClearNamedBufferSubData",
    "InvalidateBufferData", "InvalidateBufferSubData",
)
# Flagged only for a vertex or storage target (or one that is not a literal).
TARGETED = ("BindBuffer", "BufferData", "BufferSubData")
# Indexed bindings: flagged whatever the target.
INDEXED = ("BindBufferBase", "BindBufferRange", "BindBuffersBase", "BindBuffersRange")
VERTEX_STATE = (
    "GenVertexArrays", "CreateVertexArrays", "DeleteVertexArrays", "BindVertexArray",
    "VertexAttribPointer", "VertexAttribIPointer", "VertexAttribLPointer",
    "EnableVertexAttribArray", "DisableVertexAttribArray",
    "VertexAttribFormat", "VertexAttribIFormat", "VertexAttribLFormat",
    "VertexAttribBinding", "VertexAttribDivisor", "BindVertexBuffer", "BindVertexBuffers",
    "VertexBindingDivisor",
    "VertexArrayAttribFormat", "VertexArrayAttribIFormat", "VertexArrayAttribLFormat",
    "VertexArrayAttribBinding", "VertexArrayBindingDivisor", "VertexArrayVertexBuffer",
    "VertexArrayVertexBuffers", "VertexArrayElementBuffer",
    "EnableVertexArrayAttrib", "DisableVertexArrayAttrib",
    "VertexAttribP1ui", "VertexAttribP2ui", "VertexAttribP3ui", "VertexAttribP4ui",
) + tuple("VertexAttribI%d%s" % (n, t) for n in (1, 2, 3, 4) for t in ("i", "ui", "iv", "uiv")) \
  + tuple("VertexAttribL%d%s" % (n, t) for n in (1, 2, 3, 4) for t in ("d", "dv")) + tuple("VertexAttrib%d%s" % (n, t) for n in (1, 2, 3, 4) for t in ("f", "fv", "d", "dv", "s", "sv")) \
  + tuple("VertexAttrib4N" + t for t in ("bv", "sv", "iv", "ub", "ubv", "usv", "uiv"))
DRAWS = (
    "DrawArrays", "DrawArraysInstanced", "DrawArraysIndirect", "DrawArraysInstancedBaseInstance",
    "DrawElements", "DrawElementsInstanced", "DrawElementsBaseVertex", "DrawElementsIndirect",
    "DrawElementsInstancedBaseVertex", "DrawElementsInstancedBaseInstance",
    "DrawElementsInstancedBaseVertexBaseInstance",
    "DrawRangeElements", "DrawRangeElementsBaseVertex",
    "MultiDrawArrays", "MultiDrawElements", "MultiDrawArraysIndirect", "MultiDrawElementsIndirect",
    "MultiDrawElementsBaseVertex", "MultiDrawArraysIndirectCount", "MultiDrawElementsIndirectCount",
    "DrawTransformFeedback", "DrawTransformFeedbackInstanced", "DrawTransformFeedbackStream",
    "DrawTransformFeedbackStreamInstanced",
)

# (file, function without gl/_fp/suffix) -> (times it appears, why it may
# stay there).
_PP_OIT = "the OIT resolve: P6 (OIT_InitShader and the resolve are gated off under -gpu)"
_PP_DRAW = ("the OIT resolve's bufferless draw (P6); bloom's three go through R_DrawArrays (P4e) "
            "and its VAO through R_GenVertexArrays / R_BindVertexArray (P4a-1)")
_PP_UBO = ("the post-process uniform buffers: under -gpu PpVert / PpParams are mirrors "
           "(P4a-1) and BloomParams (P4e), OitParams is P6; the deletes are "
           "shutdown's, under `if (*_ubo)`")
_SHADOW = "dlight shadow maps: P6 (gated off by R_LightCluster_Available under -gpu)"
_UBO = ("uniform-block buffers: -gpu pushes gl_uniforms.c's CPU mirrors instead "
        "(RG_UniformBlockRegister), so the GL buffers behind them are never read there")
ALLOWED = {
    ("engine/h2shared/gl_postprocess.c", "BindVertexArray"): (2, _PP_OIT),
    ("engine/h2shared/gl_postprocess.c", "DeleteBuffers"): (4, _PP_UBO),
    ("engine/h2shared/gl_postprocess.c", "DrawArrays"): (1, _PP_DRAW),
    ("engine/h2shared/gl_postprocess.c", "GenBuffers"): (4, _PP_UBO),
    ("engine/h2shared/gl_postprocess.c", "GenVertexArrays"): (1, _PP_OIT),
    ("engine/h2shared/gl_shadow.c", "BindVertexArray"): (5, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "DeleteBuffers"): (1, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "DeleteVertexArrays"): (1, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "DrawArrays"): (1, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "DrawElements"): (1, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "GenBuffers"): (1, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "GenVertexArrays"): (1, _SHADOW),
    ("engine/h2shared/gl_shadow.c", "MultiDrawElements"): (2, _SHADOW),
    ("engine/h2shared/gl_uniforms.c", "DeleteBuffers"): (6, _UBO),
    ("engine/h2shared/gl_uniforms.c", "GenBuffers"): (6, _UBO),
}

NAMES = "|".join(sorted(BUFFERS + TARGETED + INDEXED + VERTEX_STATE + DRAWS, key=len, reverse=True))
NAME = r"\b(gl(" + NAMES + r")(?:ARB|EXT|OES|KHR|NV|AMD)?(?:_fp)?)\b"
CALL_RE = re.compile(NAME + r"\s*\)?\s*\(([^()]*\))?")
BARE_RE = re.compile(NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
_CAST = r"(?:\(\s*[A-Za-z_][\w\s]*\**\s*\)\s*)?"
VALUE_RE = re.compile(
    r"(?:(?<![=!<>])=|(?<!&)&(?!&))\s*" + _CAST + r"\(?\s*" + NAME + r"(?!\s*\)?\s*\()(?!\s*[!=]=)")
_NOT_CALLED = r"(?!\s*\)?\s*\()(?!\s*(?:[!=]=|&&|\|\|))"
HANDOFF_RE = re.compile(r"(?:[,?:{]|\breturn\b)\s*" + _CAST + NAME + _NOT_CALLED)
ARG_RE = re.compile(r"\b(?!(?:if|while|for|switch|return|sizeof)\b)[A-Za-z_]\w*\s*\(\s*"
                    + _CAST + NAME + _NOT_CALLED)
DEFINE_RE = re.compile(r"^[ \t]*#[ \t]*define\b(?:[^\n]*\\\n)*[^\n]*", re.MULTILINE)
BLANK_RE = re.compile(
    r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.DOTALL)
# A literal first argument naming one of these targets is another phase's
# buffer.  Listed rather than "anything but the two vertex targets", so a
# _ARB / _EXT spelling or a #define'd alias of a vertex target still counts.
OTHER_TARGET_RE = re.compile(r"\s*\)?\s*\(\s*(GL_\w+)\s*,")
OTHER_TARGETS = {"GL_UNIFORM_BUFFER", "GL_DRAW_INDIRECT_BUFFER",
                 "GL_DISPATCH_INDIRECT_BUFFER", "GL_COPY_READ_BUFFER", "GL_COPY_WRITE_BUFFER",
                 "GL_PIXEL_PACK_BUFFER", "GL_PIXEL_UNPACK_BUFFER", "GL_TEXTURE_BUFFER",
                 "GL_QUERY_BUFFER", "GL_ATOMIC_COUNTER_BUFFER", "GL_TRANSFORM_FEEDBACK_BUFFER"}


def blank(text):
    """Blank comments and literals, keeping newlines so line numbers hold."""
    return BLANK_RE.sub(
        lambda m: "".join("\n" if ch == "\n" else " " for ch in m.group(0)),
        text)


def functions(code):
    """[(name, open, close)] of each top-level function body."""
    out, depth, start = [], 0, None
    for i, c in enumerate(code):
        if c == "{":
            if depth == 0:
                m = re.search(r"(\w+)\s*\([^;{}]*\)\s*$", code[max(0, i - 400):i])
                start = (m.group(1) if m else None, i)
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start:
                out.append((start[0], start[1], i))
                start = None
    return out


def scan(rel, text, used, owner=None):
    errors = []
    spans = functions(text) if owner is not None else []

    def owned(pos):
        return any(b <= pos < e and n in owner for n, b, e in spans)

    def line(pos):
        return text.count("\n", 0, pos) + 1

    for m in CALL_RE.finditer(text):
        full, name, args = m.group(1), m.group(2), m.group(3)
        args = " ".join(args[:-1].split()) if args is not None else None
        if name in TARGETED:
            t = OTHER_TARGET_RE.match(text, m.end(1))
            if t and t.group(1) in OTHER_TARGETS:
                continue	# a uniform, indirect or pixel buffer: not P2d's or P2e's
        if owner is not None and owned(m.start()):
            continue
        key = (rel, name)
        if key in ALLOWED:
            used[key] = used.get(key, 0) + 1
            continue
        shown = args if args is not None else "..."
        errors.append(f"{rel}:{line(m.start())}: {full}({shown}) "
                      "-- buffer, vertex-array or draw call outside gl_buffer.c / gl_vbo.c")
    for d in DEFINE_RE.finditer(text):
        for m in BARE_RE.finditer(d.group(0)):
            errors.append(f"{rel}:{line(d.start() + m.start())}: #define aliases "
                          f"{m.group(1)} -- buffer, vertex-array or draw call outside the seams")
    seen = set()
    for rx in (VALUE_RE, HANDOFF_RE, ARG_RE):
        for m in rx.finditer(text):
            if m.start(1) in seen:
                continue
            seen.add(m.start(1))
            errors.append(f"{rel}:{line(m.start(1))}: {m.group(1)} taken as a value "
                          "-- buffer, vertex-array or draw call outside the seams")
    return errors


def main():
    errors, used, files = [], {}, 0
    paths = sorted(p for p in (ROOT / "engine").rglob("*") if p.suffix in SUFFIXES)
    for path in paths:
        rel = path.relative_to(ROOT).as_posix()
        if rel in EXEMPT:
            continue
        if "/rust/" in rel and "/target/" in rel:
            continue
        files += 1
        errors += scan(rel, blank(path.read_text(errors="replace")), used, OWNERS.get(rel))
    for rel, names in OWNERS.items():
        have = {n for n, _, _ in functions(blank((ROOT / rel).read_text(errors="replace")))}
        for n in sorted(names - have):
            errors.append(f"{rel}: OWNERS names {n}, which is gone -- drop it")

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
    print(f"PASS no buffer, vertex-array or draw GL call outside gl_buffer.c / gl_vbo.c in {files} "
          f"files, {left} allowlisted in {len(ALLOWED)} entries (#340 P2d, P2e)")


if __name__ == "__main__":
    main()
