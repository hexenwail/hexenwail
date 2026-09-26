#!/usr/bin/env python3
"""Gate: the generic GL buffer binding point must never be cached again.

Issue #203 and its GL_INVALID_OPERATION twin ("glBufferSubData(no buffer
bound)") were both caused by gl_buffer.c caching the non-indexed binding for
GL_SHADER_STORAGE_BUFFER and skipping a bind it believed redundant.  The cache
could not be trusted, because ~40 raw glBindBuffer_fp calls elsewhere in the
engine bypass it and change the real binding behind its back.  The fix was to
delete the cache, not to patch the call sites -- a call site that is correct
today is one refactor away from not being.

This gate is what keeps it deleted.  It is deliberately structural rather than
behavioural: reproducing the bug at runtime needs a GL context, a map, and a
frame ordering that varies by driver, while re-introducing the cache is a
source change that is trivial to see.

Reproduction the fix was verified against, for whoever reads this after a
failure:  inject into LC_UploadAndDispatch, immediately before its cached
bind, a GL_BindBufferBase naming lc_ssbo followed by a raw
glBindBuffer_fp(GL_SHADER_STORAGE_BUFFER, 0) -- the two-statement precondition
the field hits frames apart.  demo1 -> demo2 headless then logs
"GL_INVALID_OPERATION in glBufferSubData(no buffer bound)" 2947 times with the
cache present and 0 times without it.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "engine" / "h2shared" / "gl_buffer.c"

# The indexed binding points (glBindBufferBase / glBindBufferRange) ARE still
# cached, and that is safe: no raw glBindBufferBase_fp or glBindBufferRange_fp
# call exists outside gl_buffer.c, and GL_ForgetBuffer covers the one other way
# to stale them (deleting a buffer whose name GL then recycles).  So this gate
# checks two things, not one.
GENERIC_CACHE_NAMES = (
    "current_array_buffer",
    "current_element_array_buffer",
    "current_shader_storage_buffer",
    "current_uniform_buffer",
    "current_draw_indirect_buffer",
)


def body_of(text, signature):
    """Return the brace-matched body of the function opened by `signature`."""
    start = text.index(signature)
    open_brace = text.index("{", start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : i]
    raise AssertionError("unbalanced braces after %r" % signature)


def main():
    problems = []
    text = SRC.read_text()

    # 1. GL_BindBuffer must bind unconditionally: one call, no branch, no return.
    body = body_of(text, "void GL_BindBuffer (GLenum target, GLuint buffer)")
    stripped = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    stripped = re.sub(r"//[^\n]*", "", stripped)
    statements = [s.strip() for s in stripped.split(";") if s.strip()]

    if statements != ["glBindBuffer_fp (target, buffer)"]:
        problems.append(
            "GL_BindBuffer must be exactly one unconditional "
            "glBindBuffer_fp (target, buffer); found: %r" % (statements,)
        )
    for keyword in ("if", "switch", "return", "?"):
        if re.search(r"\b%s\b" % re.escape(keyword) if keyword.isalpha() else re.escape(keyword), stripped):
            problems.append(
                "GL_BindBuffer contains %r -- the generic binding point must "
                "not be conditional; see #203." % keyword
            )

    # 2. No per-target cache variable for the generic binding may come back.
    for name in GENERIC_CACHE_NAMES:
        if name in text:
            problems.append(
                "%s reintroduces a generic-binding cache in %s; see #203."
                % (name, SRC.relative_to(ROOT))
            )

    # 3. The indexed-binding cache must still be invalidated on delete, on both
    #    build tiers: gl_buffer.c is one file with a desktop-GL half and a
    #    USE_GLES stub half, and every SSBO/UBO delete site calls GL_ForgetBuffer
    #    unconditionally, so a definition missing from either half is a link
    #    error on that tier.
    tiers = text.split("#else\t/* USE_GLES")
    defined_in = [i for i, half in enumerate(tiers)
                  if "void GL_ForgetBuffer (GLuint handle)" in half]
    if len(tiers) != 2:
        problems.append(
            "%s no longer splits on the USE_GLES stub marker; this gate cannot "
            "tell the two tiers apart any more." % SRC.relative_to(ROOT)
        )
    elif defined_in != [0, 1]:
        missing = "desktop GL" if 0 not in defined_in else "USE_GLES stub"
        problems.append(
            "GL_ForgetBuffer has no definition in the %s half of %s -- deleting "
            "a buffer whose name GL recycles would leave a matching stale entry "
            "in the range cache (or fail to link)."
            % (missing, SRC.relative_to(ROOT))
        )
    if defined_in:
        drain = body_of(text, "static void GL_DrainGarbage (frameres_t *frame)")
        if "GL_ForgetBuffer" not in drain:
            problems.append(
                "GL_DrainGarbage deletes ring buffers without calling "
                "GL_ForgetBuffer; their names reach the range cache via "
                "GL_BindBufferRange in R_DrawAliasInstanced."
            )

    for p in problems:
        print("FAIL: %s" % p)
    print(
        "gl_buffer.c binding gate: %d problem(s)" % len(problems)
    )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
