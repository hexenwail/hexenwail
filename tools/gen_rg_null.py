#!/usr/bin/env python3
"""Generate engine/h2shared/rg_null.c, SDL_GPU mode's null GL table (#340 P0).

Under `-gpu` there is no GL context, but the renderer still calls GL through
the loader table in gl_func.h -- every `glFoo_fp` pointer.  rg_null.c gives
each one a counting no-op with the exact signature gl_func.h declares, so the
engine boots and runs with nothing behind GL, and `gpu_unrouted` reports
every entry point the frame still reaches.  That count is what each later
phase drives to zero.

The table is generated, not hand-written, because it has to track gl_func.h
entry for entry: a new GL_FUNCTION / GL_FUNCTION_OPT line with no stub would
leave its pointer NULL in SDL_GPU mode, and the first call would crash.  The
stub signatures come from the same lines, and RG_NullGL_Install assigns each
stub to its `_fp`, so a type that drifts is a compile error (incompatible
pointer type), not a silent mismatch.

Most stubs only count.  The ones whose result the engine reads get a body
from BODIES below: glGen* hand out fake nonzero names, glGet* write defined
values (zero, or the "yes"/limit the engine needs to keep going), compile and
link status report success, glCheckFramebufferStatus says COMPLETE.  A glGet*
or glGen* entry, or any entry with a return value, that has no BODIES entry
fails generation -- its caller would read an uninitialised out-parameter or a
garbage return.  Bodies naming an entry gl_func.h no longer has fail too.

    gen_rg_null.py            write engine/h2shared/rg_null.c
    gen_rg_null.py --check    exit 1 if the committed file is stale (lint.yml)

Stock python3.
"""
from pathlib import Path
import argparse
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
GL_FUNC = ROOT / "engine/h2shared/gl_func.h"
OUT = ROOT / "engine/h2shared/rg_null.c"

# Hand-written bodies.  Parameters are a0, a1, ... in gl_func.h order.  The
# count is added by the generator; a body only supplies the effect.
BODIES = {
    # names: nonzero, never reused, so a caller testing `if (name)` proceeds
    "glGenTextures": "rgn_gen_names (a0, a1);",
    "glGenBuffers": "rgn_gen_names (a0, a1);",
    "glGenFramebuffers": "rgn_gen_names (a0, a1);",
    "glGenRenderbuffers": "rgn_gen_names (a0, a1);",
    "glGenVertexArrays": "rgn_gen_names (a0, a1);",
    "glGenQueries": "rgn_gen_names (a0, a1);",
    "glCreateShader": "(void)a0; return ++rgn_next_name;",
    "glCreateProgram": "return ++rgn_next_name;",
    # state queries
    "glGetError": "return GL_NO_ERROR;",
    "glIsEnabled": "(void)a0; return GL_FALSE;",
    # imagedump's guard: no texture exists, so it skips the readback
    "glIsTexture": "(void)a0; return GL_FALSE;",
    "glGetString": "return (const GLubyte *) rgn_string (a0);",
    "glGetIntegerv": "rgn_get_integer (a0, a1);",
    "glGetFloatv": "rgn_get_float (a0, a1);",
    "glGetTexLevelParameteriv": "(void)a0; (void)a1; (void)a2; *a3 = 0;",
    # imagedump (behind glIsTexture above) and lightcluster_verify, whose
    # buffer is static, so it reads zeros and reports mismatches; the out
    # size depends on a texture that does not exist, so nothing is written
    "glGetTexImage": "(void)a0; (void)a1; (void)a2; (void)a3; (void)a4;",
    # screenshot, screenhash and the FBO self-test (which SDL_GPU mode
    # skips): their buffers are malloc'd, so a stub that wrote nothing would
    # hand them heap garbage to save or hash.  Zeroed at the tightly packed
    # size, which no pack state can make larger than the caller's buffer.
    "glReadPixels": "(void)a0; (void)a1; rgn_read_pixels (a2, a3, a4, a5, a6);",
    "glCheckFramebufferStatus": "(void)a0; return GL_FRAMEBUFFER_COMPLETE;",
    # programs: everything compiles and links
    "glGetShaderiv": "(void)a0; *a2 = (a1 == GL_COMPILE_STATUS) ? GL_TRUE : 0;",
    "glGetProgramiv": "(void)a0; *a2 = (a1 == GL_LINK_STATUS) ? GL_TRUE : 0;",
    "glGetShaderInfoLog": "rgn_info_log (a1, a2, a3); (void)a0;",
    "glGetProgramInfoLog": "rgn_info_log (a1, a2, a3); (void)a0;",
    "glGetUniformLocation": "(void)a0; (void)a1; return 0;",
    "glGetUniformBlockIndex": "(void)a0; (void)a1; return 0;",
    # the block-size checks compare this with the C mirror and print a
    # warning when they differ; no driver has laid the block out, so 0
    "glGetActiveUniformBlockiv": "(void)a0; (void)a1; (void)a2; *a3 = 0;",
    "glGetBufferSubData": "(void)a0; (void)a1; if (a3 && a2 > 0) memset (a3, 0, (size_t)a2);",
    # timer queries: available at once, reading 0 ns
    "glGetQueryObjectiv": "(void)a0; *a2 = (a1 == GL_QUERY_RESULT_AVAILABLE) ? GL_TRUE : 0;",
    "glGetQueryObjectui64v": "(void)a0; (void)a1; *a2 = 0;",
    # buffers and sync.  gl_vidsdl.c turns gl_buffer_storage_able off in
    # SDL_GPU mode, so nothing maps.  That flag is load-bearing: the ring's
    # mapping path (gl_buffer.c) Sys_Errors on this NULL.
    "glMapBufferRange": "(void)a0; (void)a1; (void)a2; (void)a3; return NULL;",
    "glUnmapBuffer": "(void)a0; return GL_TRUE;",
    "glFenceSync": "(void)a0; (void)a1; return (GLsync) &rgn_sync;",
    "glClientWaitSync": "(void)a0; (void)a1; (void)a2; return GL_ALREADY_SIGNALED;",
}

PROLOGUE = r'''/* rg_null.c -- SDL_GPU mode's null GL table (#340 P0)
 *
 * GENERATED by tools/gen_rg_null.py from engine/h2shared/gl_func.h.  Do not
 * edit: change the generator (or gl_func.h) and rerun it; lint.yml runs
 * `tools/gen_rg_null.py --check` and fails a stale copy.
 *
 * Every GL entry point the engine calls through gl_func.h's loader table is
 * pointed at a counting no-op here when the engine runs under -gpu, so the GL
 * renderer runs to completion with nothing behind it and `gpu_unrouted`
 * names each call that has not yet been routed to SDL_GPU.  Values the engine
 * reads back are defined, never uninitialised: see tools/gen_rg_null.py.
 *
 * Copyright (C) 2026  Contributors of the uHexen2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include "quakedef.h"
#include "rg_device.h"

#ifndef USE_SDL_GPU
#error rg_null.c belongs to the SDL_GPU backend: CMake compiles it only with USE_SDL_GPU
#endif
#if !defined(GL_DLSYM) || defined(USE_GLES)
#error rg_null.c fills the GL_DLSYM loader table; there is none on this build
#endif

/* Spelled out rather than taken from the system GL headers, which are not
 * required to carry the 4.x enums. */
#define RGN_COMPILE_STATUS		0x8B81
#define RGN_LINK_STATUS			0x8B82
#define RGN_FRAMEBUFFER_COMPLETE	0x8CD5
#define RGN_QUERY_RESULT_AVAILABLE	0x8867
#define RGN_ALREADY_SIGNALED		0x911A
#undef GL_COMPILE_STATUS
#undef GL_LINK_STATUS
#undef GL_FRAMEBUFFER_COMPLETE
#undef GL_QUERY_RESULT_AVAILABLE
#undef GL_ALREADY_SIGNALED
#define GL_COMPILE_STATUS		RGN_COMPILE_STATUS
#define GL_LINK_STATUS			RGN_LINK_STATUS
#define GL_FRAMEBUFFER_COMPLETE		RGN_FRAMEBUFFER_COMPLETE
#define GL_QUERY_RESULT_AVAILABLE	RGN_QUERY_RESULT_AVAILABLE
#define GL_ALREADY_SIGNALED		RGN_ALREADY_SIGNALED

static GLuint	rgn_next_name;
static int	rgn_sync;	/* any non-NULL GLsync */

static void rgn_gen_names (GLsizei n, GLuint *out)
{
	GLsizei	i;

	for (i = 0; i < n; i++)
		out[i] = ++rgn_next_name;
}

static void rgn_info_log (GLsizei size, GLsizei *length, char *log)
{
	if (length)
		*length = 0;
	if (log && size > 0)
		log[0] = 0;
}

/* glReadPixels writes width * height pixels, each row padded up to
 * GL_PACK_ALIGNMENT (and GL_PACK_ROW_LENGTH, which nothing sets).  Padding
 * only ever adds bytes, so the tight size is a lower bound on any caller's
 * buffer and zeroing that much can never overrun it.  A format or type this
 * does not know writes nothing.  No pixel-pack buffer is ever bound, so
 * `data` is always a client pointer. */
static void rgn_read_pixels (GLsizei w, GLsizei h, GLenum format, GLenum type, void *data)
{
	size_t	comps, bytes;

	switch (format)
	{
	case 0x1902: case 0x1903: comps = 1; break;	/* GL_DEPTH_COMPONENT, GL_RED */
	case 0x8227: comps = 2; break;			/* GL_RG */
	case 0x1907: comps = 3; break;			/* GL_RGB */
	case 0x1908: case 0x80E1: comps = 4; break;	/* GL_RGBA, GL_BGRA */
	default: return;
	}
	switch (type)
	{
	case 0x1401: bytes = 1; break;			/* GL_UNSIGNED_BYTE */
	case 0x1403: case 0x140B: bytes = 2; break;	/* GL_UNSIGNED_SHORT, GL_HALF_FLOAT */
	case 0x1405: case 0x1406: bytes = 4; break;	/* GL_UNSIGNED_INT, GL_FLOAT */
	default: return;
	}
	if (data && w > 0 && h > 0)
		memset (data, 0, (size_t)w * (size_t)h * comps * bytes);
}

static const char *rgn_string (GLenum name)
{
	switch (name)
	{
	case 0x1F00: return "Hexenwail";				/* GL_VENDOR */
	case 0x1F01: return "null GL (SDL_GPU mode)";		/* GL_RENDERER */
	case 0x1F02: return "4.3 null GL (SDL_GPU mode)";	/* GL_VERSION */
	case 0x8B8C: return "4.30 null GL";			/* GL_SHADING_LANGUAGE_VERSION */
	default:     return "";					/* GL_EXTENSIONS: none */
	}
}

/* glGet* answers.  `count` is how many values the query writes; every one is
 * written.  A pname not listed writes one zero and is recorded, so
 * `gpu_unrouted` shows a query this table does not yet answer -- one zero is
 * never an overrun, because every glGet* writes at least one value.
 *
 * The limits are the ones the engine checks before it will run.  16384 for
 * 2D and cube textures is what current desktop drivers report.  The 2D one
 * is a placeholder only: gl_max_size comes from RG_MaxTextureSize, never
 * from here (#513, tools/test_rg_max_size.py).  The cube one still reaches
 * gl_shadow.c, which clamps to 1024 first, under Vulkan's 4096 cube floor.
 * Render targets sized from the window are capped by neither.  MSAA reads 0
 * (unsupported for now), so no multisampled target is ever requested. */
typedef struct
{
	GLenum		pname;
	int		count;
	GLint		value;
} rgn_query_t;

static const rgn_query_t rgn_queries[] =
{
	{ 0x0BA2, 4, 0 },	/* GL_VIEWPORT */
	{ 0x0C10, 4, 0 },	/* GL_SCISSOR_BOX */
	{ 0x0C22, 4, 0 },	/* GL_COLOR_CLEAR_VALUE */
	{ 0x0D33, 1, 16384 },	/* GL_MAX_TEXTURE_SIZE */
	{ 0x851C, 1, 16384 },	/* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
	{ 0x8D57, 1, 0 },	/* GL_MAX_SAMPLES */
	{ 0x90DD, 1, 16 },	/* GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS */
	{ 0x90DF, 1, 256 },	/* GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT */
	{ 0x8A34, 1, 256 },	/* GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT */
	{ 0x0D05, 1, 4 },	/* GL_PACK_ALIGNMENT */
	{ 0x0CF5, 1, 4 },	/* GL_UNPACK_ALIGNMENT */
	{ 0x8CA6, 1, 0 },	/* GL_FRAMEBUFFER_BINDING, GL_DRAW_FRAMEBUFFER_BINDING */
	{ 0x8CAA, 1, 0 },	/* GL_READ_FRAMEBUFFER_BINDING */
	{ 0x8069, 1, 0 },	/* GL_TEXTURE_BINDING_2D */
	{ 0x84FF, 1, 1 },	/* GL_MAX_TEXTURE_MAX_ANISOTROPY */
};

#define RGN_MAX_UNKNOWN	32
static struct
{
	GLenum		pname;
	unsigned long	calls;
} rgn_unknown[RGN_MAX_UNKNOWN];
static int		rgn_num_unknown;
static unsigned long	rgn_unknown_overflow;

static const rgn_query_t *rgn_query (GLenum pname)
{
	int	i;

	for (i = 0; i < (int)(sizeof(rgn_queries) / sizeof(rgn_queries[0])); i++)
	{
		if (rgn_queries[i].pname == pname)
			return &rgn_queries[i];
	}
	for (i = 0; i < rgn_num_unknown; i++)
	{
		if (rgn_unknown[i].pname == pname)
		{
			rgn_unknown[i].calls++;
			return NULL;
		}
	}
	if (rgn_num_unknown < RGN_MAX_UNKNOWN)
	{
		rgn_unknown[rgn_num_unknown].pname = pname;
		rgn_unknown[rgn_num_unknown].calls = 1;
		rgn_num_unknown++;
	}
	else
		rgn_unknown_overflow++;
	return NULL;
}

static void rgn_get_integer (GLenum pname, GLint *out)
{
	const rgn_query_t	*q = rgn_query (pname);
	int			i;

	if (!q)
	{
		out[0] = 0;
		return;
	}
	for (i = 0; i < q->count; i++)
		out[i] = q->value;
}

static void rgn_get_float (GLenum pname, GLfloat *out)
{
	const rgn_query_t	*q = rgn_query (pname);
	int			i;

	if (!q)
	{
		out[0] = 0.0f;
		return;
	}
	for (i = 0; i < q->count; i++)
		out[i] = (GLfloat) q->value;
}
'''

EPILOGUE = r'''
static int rgn_cmp (const void *a, const void *b)
{
	unsigned long	ca = rgn_calls[*(const int *)a], cb = rgn_calls[*(const int *)b];

	if (ca != cb)
		return (ca < cb) ? 1 : -1;
	return strcmp (rgn_names[*(const int *)a], rgn_names[*(const int *)b]);
}

unsigned long RG_NullGL_Total (int *entry_points)
{
	unsigned long	total = 0;
	int		i, hit = 0;

	for (i = 0; i < RGN_COUNT; i++)
	{
		total += rgn_calls[i];
		hit += (rgn_calls[i] != 0);
	}
	if (entry_points)
		*entry_points = hit;
	return total;
}

/* gpu_unrouted [reset] -- every GL entry point the null table has absorbed
 * since startup (or the last reset), most-called first. */
void RG_NullGL_Report_f (void)
{
	int		order[RGN_COUNT];
	int		i, n = 0, hit;
	unsigned long	total;

	if (Cmd_Argc () > 1 && !q_strcasecmp (Cmd_Argv (1), "reset"))
	{
		memset (rgn_calls, 0, sizeof(rgn_calls));
		for (i = 0; i < rgn_num_unknown; i++)
			rgn_unknown[i].calls = 0;
		rgn_unknown_overflow = 0;
		Con_Printf ("gpu_unrouted: counters reset\n");
		return;
	}
	if (!R_GPU ())
	{
		Con_Printf ("gpu_unrouted: not in SDL_GPU mode (start with -gpu)\n");
		return;
	}
	for (i = 0; i < RGN_COUNT; i++)
	{
		if (rgn_calls[i])
			order[n++] = i;
	}
	qsort (order, n, sizeof(order[0]), rgn_cmp);
	for (i = 0; i < n; i++)
		Con_Printf ("%10lu  %s\n", rgn_calls[order[i]], rgn_names[order[i]]);
	for (i = 0; i < rgn_num_unknown; i++)
	{
		if (rgn_unknown[i].calls)
			Con_Printf ("%10lu  (unanswered glGet pname 0x%04X)\n",
				    rgn_unknown[i].calls, (unsigned int) rgn_unknown[i].pname);
	}
	if (rgn_unknown_overflow)
		Con_Printf ("%10lu  (unanswered glGet pnames past the first %d)\n",
			    rgn_unknown_overflow, RGN_MAX_UNKNOWN);
	total = RG_NullGL_Total (&hit);
	Con_Printf ("gpu_unrouted: %lu calls into %d of %d GL entry points\n",
		    total, hit, RGN_COUNT);
}
'''


def strip_comments(text):
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
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def split_params(params):
    """'(GLenum, const GLuint *)' -> ['GLenum', 'const GLuint *']; () or (void) -> []."""
    inner = params.strip()
    if not (inner.startswith("(") and inner.endswith(")")):
        raise ValueError("parameter list %r is not parenthesised" % params)
    inner = inner[1:-1].strip()
    if inner in ("", "void"):
        return []
    parts, depth, cur = [], 0, ""
    for c in inner:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += c
    parts.append(cur.strip())
    if any(not p or "(" in p for p in parts):
        raise ValueError("cannot name the parameters of %r" % params)
    return parts


ENTRY = re.compile(r"\b(GL_FUNCTION|GL_FUNCTION_OPT)\s*\(")


def parse(text):
    """Entries in gl_func.h's desktop loader table, each with the #if stack
    (inside that table) it sits under: [(kind, ret, name, [ptypes], [conds])]."""
    clean = strip_comments(text)
    lines = clean.split("\n")
    entries, seen = [], set()
    # Region: the GL_DLSYM branch (until its #elif) plus the shared
    # GL_FUNCTION_OPT list (until #undef GL_FUNCTION_OPT).
    region = None
    stack = []        # conditionals opened inside the region
    depth = 0         # all conditionals, for finding the region's end
    region_depth = None
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        d = re.match(r"#\s*(\w+)\s*(.*)", s)
        if d:
            kw, rest = d.group(1), " ".join(d.group(2).split())
            if kw in ("if", "ifdef", "ifndef"):
                depth += 1
                if region is None and kw == "if" and rest == "defined(GL_DLSYM) && !defined(USE_GLES)":
                    region, region_depth = "dlsym", depth
                elif region:
                    stack.append("#%s %s" % (kw, rest))
            elif kw in ("elif", "else"):
                if region == "dlsym" and depth == region_depth:
                    region = None
                elif region:
                    if stack:
                        raise ValueError("gl_func.h:%d: #%s inside the loader table is not "
                                         "supported by gen_rg_null.py" % (i + 1, kw))
            elif kw == "endif":
                if region and stack and not (region == "dlsym" and depth == region_depth):
                    stack.pop()
                depth -= 1
            elif kw == "undef" and rest == "GL_FUNCTION_OPT":
                region = None
            elif kw == "define":
                pass
            i += 1
            continue
        if region is None and "GL_FUNCTION_OPT(" in s.replace(" ", "") and not s.startswith("#"):
            region, stack = "opt", []
        if region:
            for m in ENTRY.finditer(line):
                # gather the full macro call, which may span lines
                buf, j, k = line[m.end():], i, 0
                while buf.count("(") + 1 > buf.count(")"):
                    j += 1
                    buf += " " + lines[j]
                depth_p, end = 1, None
                for k, c in enumerate(buf):
                    depth_p += (c == "(") - (c == ")")
                    if depth_p == 0:
                        end = k
                        break
                args = buf[:end]
                am = re.match(r"\s*([^,]+?)\s*,\s*(\w+)\s*,\s*(\(.*\))\s*$", args, re.S)
                if not am:
                    raise ValueError("gl_func.h:%d: cannot parse %s(%s)" % (i + 1, m.group(1), args))
                ret, name, params = " ".join(am.group(1).split()), am.group(2), am.group(3)
                if name in seen:
                    raise ValueError("gl_func.h:%d: %s declared twice in the loader table" % (i + 1, name))
                seen.add(name)
                entries.append((m.group(1), ret, name, split_params(" ".join(params.split())), list(stack)))
        i += 1
    return entries


def declare(ptype, idx):
    return "%s a%d" % (ptype, idx)


def generate():
    text = GL_FUNC.read_text()
    entries = parse(text)
    if not entries:
        raise ValueError("no GL_FUNCTION entries found in gl_func.h")
    # parse() finds the loader table by its #if text; if that drifts, whole
    # regions drop out silently and their pointers stay NULL under -gpu.
    # Every GL_FUNCTION / GL_FUNCTION_OPT call in the file must be an entry.
    calls = [l for l in strip_comments(text).split("\n")
             if re.match(r"\s*GL_FUNCTION(?:_OPT)?\s*\(", l)]
    if len(calls) != len(entries):
        raise ValueError("gl_func.h has %d GL_FUNCTION / GL_FUNCTION_OPT calls but the loader "
                         "table parse found %d: its #if structure changed under gen_rg_null.py"
                         % (len(calls), len(entries)))
    names = {e[2] for e in entries}
    errs = []
    for kind, ret, name, ptypes, conds in entries:
        needs = ret != "void" or name.startswith("glGet") or name.startswith("glGen") or \
            name.startswith("glCreate") or name.startswith("glMap") or name == "glReadPixels"
        if needs and name not in BODIES:
            errs.append("%s (returns %s) needs a hand-written body in gen_rg_null.py BODIES: "
                        "its caller reads what it produces" % (name, ret))
    for name in BODIES:
        if name not in names:
            errs.append("BODIES names %s, which gl_func.h's loader table does not declare" % name)
    if errs:
        raise ValueError("\n".join(errs))

    out = [PROLOGUE]

    def wrap(conds, text):
        if not conds:
            return text
        return "\n".join(conds) + "\n" + text + "\n" + "\n".join("#endif" for _ in conds)

    out.append("\nenum\n{")
    for kind, ret, name, ptypes, conds in entries:
        out.append(wrap(conds, "\tRGN_%s," % name))
    out.append("\tRGN_COUNT\n};\n")
    out.append("static unsigned long\trgn_calls[RGN_COUNT];\n")
    out.append("static const char *const rgn_names[RGN_COUNT] =\n{")
    for kind, ret, name, ptypes, conds in entries:
        out.append(wrap(conds, '\t[RGN_%s] = "%s",' % (name, name)))
    out.append("};\n")

    for kind, ret, name, ptypes, conds in entries:
        plist = ", ".join(declare(p, k) for k, p in enumerate(ptypes)) or "void"
        if name in BODIES:
            body = "\t" + BODIES[name]
        else:
            voids = " ".join("(void)a%d;" % k for k in range(len(ptypes)))
            body = "\t" + voids if voids else ""
        fn = "static %s APIENTRY rgn_%s (%s)\n{\n\trgn_calls[RGN_%s]++;\n%s\n}\n" % (
            ret, name, plist, name, body.rstrip() if body.strip() else "")
        fn = fn.replace("\n\n}", "\n}")
        out.append(wrap(conds, fn.rstrip("\n")) + "\n")

    out.append("/* Point every loader-table entry at its stub.  The assignment is what\n"
               " * type-checks the stub against gl_func.h's typedef. */\n"
               "void RG_NullGL_Install (void)\n{")
    for kind, ret, name, ptypes, conds in entries:
        out.append(wrap(conds, "\t%s_fp = rgn_%s;" % (name, name)))
    out.append("}\n")
    out.append("/* Entries that no longer point at their stub, each named on the console.\n"
               " * Any later SDL_GL_GetProcAddress load overwrites a stub with NULL, since\n"
               " * -gpu loads no GL library; GL_Init_NullGL refuses to run past one. */\n"
               "int RG_NullGL_Verify (void)\n{\n\tint\tbad = 0;\n\n"
               "#define RGN_VERIFY(f) \\\n"
               "\tif (f##_fp != rgn_##f) \\\n"
               "\t{ \\\n"
               "\t\tCon_SafePrintf (\"null GL: %s_fp was overwritten after RG_NullGL_Install\\n\", #f); \\\n"
               "\t\tbad++; \\\n"
               "\t}")
    for kind, ret, name, ptypes, conds in entries:
        out.append(wrap(conds, "\tRGN_VERIFY (%s)" % name))
    out.append("#undef RGN_VERIFY\n\treturn bad;\n}\n")
    out.append(EPILOGUE.lstrip("\n"))
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--check", action="store_true", help="fail if rg_null.c is stale")
    ap.add_argument("--root", type=Path, help="tree to read (default: this checkout)")
    a = ap.parse_args(argv)
    global GL_FUNC, OUT
    if a.root:
        GL_FUNC = a.root / "engine/h2shared/gl_func.h"
        OUT = a.root / "engine/h2shared/rg_null.c"
    try:
        text = generate()
    except ValueError as e:
        print("FAIL: gen_rg_null: %s" % e)
        return 1
    if a.check:
        if not OUT.is_file() or OUT.read_text() != text:
            print("FAIL: %s is stale; run tools/gen_rg_null.py" % OUT.relative_to(OUT.parents[2]))
            return 1
        print("gen_rg_null: OK, rg_null.c matches gl_func.h (%d entry points)"
              % text.count("_fp = rgn_"))
        return 0
    OUT.write_text(text)
    print("gen_rg_null: wrote %s (%d entry points)" % (OUT, text.count("_fp = rgn_")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
