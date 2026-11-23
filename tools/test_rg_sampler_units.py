#!/usr/bin/env python3
"""Every sampler gets its texture unit through one seam, and the SDL_GPU
backend reads that unit (GitHub #340, phase P2a).

GL learns which texture unit a sampler reads from glUniform1i at program
set-up; SDL_GPU binds a sampler by its slot in the SPIR-V, which says
nothing about the unit the renderer put the texture on (salias_frag's
u_lightgrid is slot 4 and unit LIGHT_GRID_TMU 5).  So both go through
R_ProgramSamplerLoc / R_ProgramSamplerUnit (gl_shader.c): the GL arm is the
glGetUniformLocation / glUniform1i pair, call for call, the SDL_GPU arm
writes the unit into the program's reflected samplers.  Checked, stock
python3 plus a C compiler (lint.yml):

  the seam
    * R_ProgramSamplerLoc's GL arm is `return glGetUniformLocation_fp
      (prog, name);` and R_ProgramSamplerUnit's does nothing for loc < 0 and
      is then `glUniform1i_fp (loc, unit);`; each SDL_GPU arm is under
      #ifdef USE_SDL_GPU / if (R_GPU ()) and calls RG_ProgramSampler /
      RG_ProgramSamplerUnit with the same arguments.
    * no other glGetUniformLocation or glUniform1i call in the engine,
      except gl_postprocess.c's OIT resolve (P6) and gl_ssao.c (P5), raw GL
      until those phases route their programs (RAW_OK, by function where it
      names them; each entry must still have such calls);
  the call sites
    * each R_ProgramSamplerUnit (prog, loc, "name", unit) names the sampler
      its location was looked up for -- `p->u_x` was assigned from
      R_ProgramSamplerLoc (..., "u_x"), a local from the nearest lookup
      before it in the function, or a file-scope location the same file
      assigned from R_ProgramSamplerLoc (same program, same "u_x") once at
      set-up, which a per-frame re-set may then name -- and its unit is the
      constant that sampler
      is bound at (CANON: u_textureN literally N, u_lightgrid LIGHT_GRID_TMU,
      the lightstyle layers LMSTYLE_TMU + k, the MBOIT targets MBOIT_TMU_*,
      the shadow maps DLSHADOW_TMU, u_soft_depth 1), each in 0..15;
  the programs
    * each graphics program in gl_shader.c's rprog_defs is linked to the
      function that sets it up (a call passing its id, or an
      R_Program_Create / SH_LinkProgram of it in the body), and the samplers
      its stages declare -- the engine/shaders sources run through the GL
      preprocessor with the row's #defines, checked against
      gl_progid_refl.h's counts -- each get a unit from that function or a
      helper it calls (R_Shadow_InitProgram), except UNSET_OK;
    * no two samplers of one program share a unit (two sampler types on one
      unit fail every GL draw; one type twice reads one texture as two);
    * every name a set-up function assigns is a sampler of one of its
      programs, so a misspelt name cannot pass as "not declared here";
    * a graphics program with samplers and no set-up function is one of
      RAW_PROGRAMS (P5 / P6);
  the backend (rg_shader.c, rg_resource.c, rg_frame.c), compiled and run
    * RG_Reflect, lifted with a stub prelude and fed SPIR-V assembled here:
      each sampler's dimensionality (cube arrays included), sampled class
      (float / int / uint), shadow and multisample flags, and its unit --
      u_textureN N, every other sampler RG_UNIT_NONE, never its binding;
    * RG_ProgramSamplerUnit sets the unit in both stages and nothing else,
      RG_ProgramSampler finds a sampler in either stage or says -1;
    * RG_Res_ForDraw binds the placeholder of exactly the sampler's kind
      (float / int / uint per dimensionality, the depth cube array with the
      compare sampler for a shadow sampler), refuses a real texture whose
      format the sampler cannot read, and drops a draw that has nothing to
      bind; the placeholder formats are the classes they stand for;
    * RGF_RecordStage notes an unmapped sampler at draw time, and every
      read-only storage-buffer slot a stage's reflection row declares is
      bound at replay: to the buffer at its block's GL binding point since
      P2e (tools/test_rg_sources.py), else to the stand-in.

Usage:  test_rg_sampler_units.py [--root DIR] [-v]
"""
from pathlib import Path
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SHADER_C = "engine/h2shared/gl_shader.c"
SHADOW_C = "engine/h2shared/gl_shadow.c"
RG_SHADER_C = "engine/h2shared/rg_shader.c"
RG_RES_C = "engine/h2shared/rg_resource.c"
RG_FRAME_C = "engine/h2shared/rg_frame.c"
RG_INTERNAL_H = "engine/h2shared/rg_internal.h"
SHADERS = "engine/shaders"
CONST_HEADERS = ("engine/h2shared/gl_shader.h", "engine/h2shared/gl_lightcluster.h",
                 "engine/h2shared/gl_shadow.h", "engine/h2shared/gl_texbind.h")
SCAN_DIRS = ("engine/h2shared", "engine/hexen2", "engine/hexenworld")
# Not engine call sites: the loader table, the loader, the generated null-GL
# table, and the WEBSOFT canvas blit (raw GLES, outside the renderer).
NOT_SITES = {"engine/h2shared/gl_func.h", "engine/h2shared/gl_vidsdl.c",
             "engine/h2shared/rg_null.c", "engine/h2shared/web_canvas_gl2.c"}
# Raw GL sampler set-up, by phase: their programs do not draw under -gpu yet.
# file -> (why, the functions it may sit in, or None for anywhere in the file).
RAW_OK = {
    "engine/h2shared/gl_postprocess.c": ("the OIT resolve, P6 on #340 (gated off under -gpu)",
                                         {"OIT_InitShader", "OIT_EndTranslucency"}),
    "engine/h2shared/gl_ssao.c": ("SSAO, compute and its composite, P5 on #340", None),
}
# The programs those files set up, which therefore have no seam set-up.  The
# post-process and bloom programs went through the seam at P4a-1.
RAW_PROGRAMS = {"oit_resolve", "oit_resolve_msaa", "ssao_composite"}
# Files whose functions set sampler units through the seam.
SETUP_FILES = (SHADER_C, SHADOW_C, "engine/h2shared/gl_postprocess.c")

# The unit each sampler is bound at, as the C spells it.
CANON = {"u_texture%d" % n: str(n) for n in range(5)}
CANON.update({
    "u_lightgrid": "LIGHT_GRID_TMU",
    "u_lmlayer1": "LMSTYLE_TMU", "u_lmlayer2": "LMSTYLE_TMU + 1", "u_lmlayer3": "LMSTYLE_TMU + 2",
    "u_mboit_b0": "MBOIT_TMU_B0", "u_mboit_mom": "MBOIT_TMU_MOM",
    "u_mboit_b0_ms": "MBOIT_TMU_B0_MS", "u_mboit_mom_ms": "MBOIT_TMU_MOM_MS",
    "u_dlshadowmap": "DLSHADOW_TMU", "u_shadowdbg": "DLSHADOW_TMU",
    # unit 1, which no program declaring it also uses (gl_shader.c, uhexen2-mf9u)
    "u_soft_depth": "1",
    # the post-process chain (gl_postprocess.c, #340 P4a-1): the composited
    # scene, the softemu palette LUT and the bloom result; each bloom pass
    # reads its source at 0
    "scene": "0", "paletteLUT": "1", "u_bloom": "2", "u_scene": "0",
})
# (program, sampler) left without a unit, and why that is right.
UNSET_OK = {
    ("alias_instanced", "u_soft_depth"):
        "GL leaves it at unit 0 and the instanced path never samples it (u_soft_params.x is 0 "
        "on every non-sprite draw); under -gpu a draw that reads it gets a placeholder",
    ("alias_instanced_np", "u_soft_depth"): "the affine twin of the above",
}

errors = []


def fail(msg):
    errors.append(msg)


def strip_c(text):
    """Comments blanked, newlines kept."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"/\*.*?\*/|//[^\n]*", blank, text, flags=re.S)


def match_close(text, i, o="{", c="}"):
    depth = 0
    for j in range(i, len(text)):
        if text[j] == o:
            depth += 1
        elif text[j] == c:
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def functions(code):
    """name -> body text (braces included), for top-level definitions."""
    out = {}
    for m in re.finditer(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\(([^;{}]*)\)\s*\{", code, re.M):
        start = m.end() - 1
        end = match_close(code, start)
        if end > 0 and m.group(1) not in ("if", "while", "for", "switch"):
            out.setdefault(m.group(1), code[start:end])
    return out


def lift(code, head_rx, what):
    """The text of a top-level definition starting at head_rx, through its
    closing brace (and `;` for a typedef/enum)."""
    m = re.search(head_rx, code, re.M)
    if not m:
        fail("cannot lift %s" % what)
        return ""
    i = code.index("{", m.start())
    j = match_close(code, i)
    k = j
    while k < len(code) and code[k] != ";" and code[k] != "\n":
        k += 1
    if k < len(code) and code[k] == ";":
        j = k + 1
    return code[m.start():j]


def split_args(s):
    args, depth, cur = [], 0, ""
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        args.append(cur.strip())
    return args


def calls(body, name):
    """(start, [args]) for every call of name in body."""
    out = []
    for m in re.finditer(r"\b%s\s*\(" % re.escape(name), body):
        i = m.end() - 1
        j = match_close(body, i, "(", ")")
        out.append((m.start(), split_args(body[i + 1:j - 1])))
    return out


# --------------------------------------------------------------------------
# Constants
# --------------------------------------------------------------------------

def constants(root):
    defs = {}
    for rel in CONST_HEADERS:
        for m in re.finditer(r"^\s*#\s*define\s+(\w+)\s+(\d+)\b", (root / rel).read_text(), re.M):
            defs[m.group(1)] = int(m.group(2))
    return defs


def unit_value(expr, defs):
    toks = re.findall(r"\w+|\+", expr)
    total, sign = 0, 1
    for t in toks:
        if t == "+":
            continue
        if t.isdigit():
            total += int(t)
        elif t in defs:
            total += defs[t]
        else:
            return None
    return total


# --------------------------------------------------------------------------
# A GLSL preprocessor, as far as engine/shaders needs one
# --------------------------------------------------------------------------

SAMPLER_DECL = re.compile(
    r"^\s*(?:(?:FRAG_TEX|TEXBIND2?|layout)\s*\([^)]*\)\s*)?uniform\s+(?:(?:highp|mediump|lowp)\s+)?"
    r"([iu]?sampler\w+)\s+(\w+)\s*;")


def cond_value(expr, macros):
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)|defined\s+(\w+)",
                  lambda m: "1" if (m.group(1) or m.group(2)) in macros else "0", expr)
    expr = re.sub(r"[A-Za-z_]\w*",
                  lambda m: str(int(macros[m.group(0)])) if m.group(0) in macros
                  and re.fullmatch(r"\d+", macros[m.group(0)] or "") else "0", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}))
    except Exception:
        raise ValueError("cannot evaluate #if %r" % expr)


def preprocess(path, macros, out, depth=0):
    """Appends the active source lines of path (includes expanded) to out."""
    if depth > 16:
        raise ValueError("include depth at %s" % path)
    stack = []      # (taking, any_taken)
    text = strip_c(path.read_text())
    for line in text.split("\n"):
        d = re.match(r"\s*#\s*(\w+)\s*(.*)$", line)
        active = all(t for t, _ in stack)
        if d:
            kw, rest = d.group(1), d.group(2).strip()
            if kw in ("ifdef", "ifndef"):
                v = (rest.split()[0] in macros) == (kw == "ifdef")
                stack.append((v and active, v))
                continue
            if kw == "if":
                v = active and cond_value(rest, macros)
                stack.append((v, v))
                continue
            if kw == "elif":
                taking, taken = stack.pop()
                outer = all(t for t, _ in stack)
                v = outer and not taken and cond_value(rest, macros)
                stack.append((v, taken or v))
                continue
            if kw == "else":
                taking, taken = stack.pop()
                outer = all(t for t, _ in stack)
                stack.append((outer and not taken, True))
                continue
            if kw == "endif":
                stack.pop()
                continue
            if not active:
                continue
            if kw == "define":
                m = re.match(r"(\w+)(\([^)]*\))?\s*(.*)$", rest)
                macros[m.group(1)] = None if m.group(2) else m.group(3).strip()
            elif kw == "undef":
                macros.pop(rest.split()[0], None)
            elif kw == "include":
                inc = re.match(r'"([^"]+)"', rest)
                preprocess(path.parent / inc.group(1), macros, out, depth + 1)
            continue
        if active:
            out.append(line)
    if stack:
        raise ValueError("%s: unterminated #if" % path)


def stage_samplers(root, body_sym, defines):
    src = root / SHADERS / (body_sym + ".glsl")
    if not src.exists():
        fail("%s: no %s" % (body_sym, src.relative_to(root)))
        return []
    macros = {k: v for k, v in defines.items()}
    lines = []
    try:
        preprocess(src, macros, lines)
    except ValueError as e:
        fail("%s: %s" % (src.relative_to(root), e))
        return []
    out = []
    for ln in lines:
        m = SAMPLER_DECL.match(ln)
        if m:
            out.append((m.group(2), m.group(1)))
    return out


# --------------------------------------------------------------------------
# The program table
# --------------------------------------------------------------------------

def program_rows(code):
    """id -> (name, link, [(header expr, body symbol, blob)])."""
    m = re.search(r"static const rprog_def_t rprog_defs\[RPROG_COUNT\] = \{", code)
    if not m:
        fail("gl_shader.c: no rprog_defs table")
        return {}
    i = code.index("{", m.end() - 1)
    table = code[i + 1:match_close(code, i) - 1]
    rows = {}
    for r in re.finditer(r"\[(RPROG_\w+)\]\s*=\s*\{\s*\"(\w+)\"\s*,\s*(RPROG_LINK_\w+)\s*,\s*\{", table):
        j = table.index("{", r.end() - 1)
        inner = table[j + 1:match_close(table, j) - 1]
        stages = [(s.group(1).strip(), s.group(2), s.group(3)) for s in
                  re.finditer(r"\{\s*([^{}]*?),\s*(\w+)\s*,\s*\"([\w.]+)\"\s*\}", inner)]
        rows[r.group(1)] = (r.group(2), r.group(3), stages)
    if len(rows) < 40:
        fail("gl_shader.c: only %d rprog_defs rows parsed; the scan is broken" % len(rows))
    return rows


def header_defines(header):
    d = {}
    if "GLSL_NOPERSP" in header:
        d["NOPERSP"] = "1"
    if "GLSL_OIT_TWIN" in header:
        d["OIT"] = "1"
    for m in re.finditer(r'#define (\w+) (\w+)\\n', header):
        d[m.group(1)] = m.group(2)
    return d


def refl_rows(root):
    t = (root / "engine/h2shared/gl_progid_refl.h").read_text()
    return {m.group(1): int(m.group(2)) for m in
            re.finditer(r'\{ "([\w.]+)", RPROG_REFL_\w+, (\d+),', t)}


# --------------------------------------------------------------------------
# The C side: the seam, the call sites, the set-up functions
# --------------------------------------------------------------------------

def norm(s):
    return " ".join(s.split())


def check_seam(code):
    fns = functions(code)
    loc = fns.get("R_ProgramSamplerLoc", "")
    unit = fns.get("R_ProgramSamplerUnit", "")
    if norm(loc) != norm("""{
#ifdef USE_SDL_GPU
	if (R_GPU ())
		return RG_ProgramSampler (prog, name);
#endif
	return glGetUniformLocation_fp (prog, name);
}"""):
        fail("R_ProgramSamplerLoc must be the SDL_GPU arm (RG_ProgramSampler) under #ifdef USE_SDL_GPU, "
             "then exactly `return glGetUniformLocation_fp (prog, name);`")
    if norm(unit) != norm("""{
	if (loc < 0)
		return;
#ifdef USE_SDL_GPU
	if (R_GPU ())
	{
		RG_ProgramSamplerUnit (prog, name, unit);
		return;
	}
#endif
	(void) prog;
	(void) name;
	glUniform1i_fp (loc, unit);
}"""):
        fail("R_ProgramSamplerUnit must return for loc < 0, take the SDL_GPU arm "
             "(RG_ProgramSamplerUnit) under #ifdef USE_SDL_GPU, then be exactly `glUniform1i_fp (loc, unit);`")


RAW_CALL = re.compile(r"\b(glGetUniformLocation|glUniform1i)(?:_fp)?\s*\(")


def check_routing(root):
    seen = {rel: 0 for rel in RAW_OK}
    for d in SCAN_DIRS:
        for p in sorted((root / d).rglob("*")):
            if p.suffix not in (".c", ".h"):
                continue
            rel = p.relative_to(root).as_posix()
            if rel in NOT_SITES:
                continue
            code = strip_c(p.read_text(errors="replace"))
            spans = []
            fns = functions(code)
            if rel == SHADER_C:
                for f in ("R_ProgramSamplerLoc", "R_ProgramSamplerUnit"):
                    b = fns.get(f)
                    if b:
                        i = code.index(b)
                        spans.append((i, i + len(b)))
            raw_spans = None
            if rel in RAW_OK and RAW_OK[rel][1] is not None:
                raw_spans = []
                for f in sorted(RAW_OK[rel][1]):
                    b = fns.get(f)
                    if b is None:
                        fail("RAW_OK names %s in %s, which has no such function" % (f, rel))
                        continue
                    i = code.index(b)
                    raw_spans.append((i, i + len(b)))
            for m in RAW_CALL.finditer(code):
                if any(a <= m.start() < b for a, b in spans):
                    continue
                if rel in RAW_OK and (raw_spans is None or
                                      any(a <= m.start() < b for a, b in raw_spans)):
                    seen[rel] += 1
                    continue
                line = code.count("\n", 0, m.start()) + 1
                fail("%s:%d: %s outside the sampler seam -- use R_ProgramSamplerLoc / "
                     "R_ProgramSamplerUnit (gl_progid.h)" % (rel, line, m.group(1)))
    for rel, n in seen.items():
        if n == 0:
            fail("RAW_OK names %s, which no longer sets up a sampler by hand -- drop the entry" % rel)


def setup_functions(root, rows):
    """name -> (file, body, [assignment (prog, name, unit expr)], {program ids})."""
    files = {rel: strip_c((root / rel).read_text()) for rel in SETUP_FILES}
    allfns = {}
    for rel, code in files.items():
        for name, body in functions(code).items():
            allfns[name] = (rel, body)
    # program variable -> the ids it is created from, per function, so a
    # local `prog` in two functions is never read as one variable
    create_rx = r"\b(\w+)\s*=\s*R_Program_Create\s*\(\s*(RPROG_\w+)\s*\)"
    created_in = {}     # (file, variable) -> {function: {ids}}
    for fname, (rel, body) in allfns.items():
        for var, pid in re.findall(create_rx, body):
            created_in.setdefault((rel, var), {}).setdefault(fname, set()).add(pid)
    if "R_ProgramSamplerUnit" not in allfns:
        fail("gl_shader.c: no R_ProgramSamplerUnit")
    out = {}
    for name, (rel, body) in allfns.items():
        if name in ("R_ProgramSamplerUnit", "R_ProgramSamplerLoc"):
            continue
        sets = calls(body, "R_ProgramSamplerUnit")
        if not sets:
            continue
        assigns = []
        for pos, a in sets:
            if len(a) != 4 or not re.fullmatch(r'"\w+"', a[2]):
                fail("%s: R_ProgramSamplerUnit (%s) is not (prog, loc, \"name\", unit)" % (name, ", ".join(a)))
                continue
            assigns.append((pos, a[0], a[1], a[2].strip('"'), a[3]))
        ids = set(re.findall(r"\b(?:R_Program_Create|SH_LinkProgram)\s*\(\s*(RPROG_\w+)\s*\)", body))
        # a per-frame re-set names the program variable its set-up created:
        # this function's own creation of it, else the one function that
        # creates it (a file-scope program)
        for _, prog, _, _, _ in assigns:
            where = created_in.get((rel, prog), {})
            if name in where:
                ids |= where[name]
            elif len(where) == 1:
                ids |= next(iter(where.values()))
        out[name] = [rel, body, assigns, ids]
    # ids passed to a set-up function by its callers
    for caller, (rel, body) in allfns.items():
        for name in out:
            for _, a in calls(body, name):
                ids = [x for x in a if re.fullmatch(r"RPROG_\w+", x)]
                out[name][3].update(ids)
    return out, allfns


def check_call_sites(setups, allfns, defs):
    for fn, (rel, body, assigns, _) in setups.items():
        for pos, prog, loc, name, unit in assigns:
            # the location was looked up for this name
            fld = re.fullmatch(r"(?:\w+(?:->|\.))*(?:base\.)?(u_\w+)", loc)
            if fld and ("->" in loc or "." in loc):
                if fld.group(1) != name:
                    fail('%s: R_ProgramSamplerUnit (%s, %s, "%s", ...) points one sampler with '
                         "another's location" % (fn, prog, loc, name))
                looked = [b for _, (r, b) in allfns.items()
                          if re.search(r"\b%s\s*=\s*R_ProgramSamplerLoc\s*\(\s*[^,]+,\s*\"%s\"\s*\)"
                                       % (re.escape(fld.group(1)), name), b)]
                if not looked:
                    fail('%s: %s is never assigned R_ProgramSamplerLoc (..., "%s")' % (fn, loc, name))
            else:
                lookup = r"\b%s\s*=\s*R_ProgramSamplerLoc\s*\(\s*([^,]+),\s*\"(\w+)\"\s*\)" % re.escape(loc)
                prior = [m for m in re.finditer(lookup, body[:pos])]
                if prior and prior[-1].group(1).strip() != prog.strip():
                    fail("%s: R_ProgramSamplerUnit (%s, %s, ...) but %s was looked up in %s"
                         % (fn, prog, loc, loc, prior[-1].group(1).strip()))
                if not prior and not re.search(r"\b%s\s*=" % re.escape(loc), body):
                    # a file-scope location, looked up once at set-up
                    rel_fn = allfns[fn][0]
                    once = [m for f, (r, b) in allfns.items() if r == rel_fn and f != fn
                            for m in re.finditer(lookup, b)]
                    if len(once) > 1:
                        fail("%s: %s is assigned R_ProgramSamplerLoc in more than one function"
                             % (fn, loc))
                    elif once and once[0].group(1).strip() != prog.strip():
                        fail("%s: R_ProgramSamplerUnit (%s, %s, ...) but %s was looked up in %s"
                             % (fn, prog, loc, loc, once[0].group(1).strip()))
                    prior = once
                if not prior:
                    fail("%s: %s is not a location R_ProgramSamplerLoc returned in this function" % (fn, loc))
                elif prior[-1].group(2) != name:
                    fail('%s: R_ProgramSamplerUnit (..., %s, "%s", ...) but %s was looked up for "%s"'
                         % (fn, loc, name, loc, prior[-1].group(2)))
            # its unit is the constant the sampler is bound at
            want = CANON.get(name)
            if want is None:
                fail('%s: sampler "%s" has no unit in CANON -- add the constant it is bound at' % (fn, name))
            elif norm(unit) != want:
                fail('%s: sampler "%s" gets unit `%s`; it is bound at %s' % (fn, name, unit, want))
            v = unit_value(unit, defs)
            if v is None or not 0 <= v < defs.get("RT_MAX_TEXTURE_UNITS", 16):
                fail('%s: sampler "%s" unit `%s` is not a texture unit 0..15' % (fn, name, unit))


def check_programs(root, rows, setups, defs):
    refl = refl_rows(root)
    by_id = {}
    for fn, (rel, body, assigns, ids) in setups.items():
        for i in ids:
            by_id.setdefault(i, []).append(fn)
    helpers = {fn for fn in setups if not setups[fn][3]}
    checked = 0
    for pid, (pname, link, stages) in sorted(rows.items()):
        if link == "RPROG_LINK_COMPUTE":
            continue        # GL fixes compute units with TEXBIND2; SDL_GPU compute is P5
        samplers = []
        for header, body_sym, blob in stages:
            got = stage_samplers(root, body_sym, header_defines(header))
            if blob in refl and refl[blob] != len(got):
                fail("%s: %d samplers declared for GL, gl_progid_refl.h's %s row has %d -- the "
                     "preprocessing here is wrong" % (pname, len(got), blob, refl[blob]))
            samplers += got
        if not samplers:
            continue
        fns = by_id.get(pid, [])
        if not fns:
            if pname not in RAW_PROGRAMS:
                fail("%s declares samplers %s and no function sets their units through "
                     "R_ProgramSamplerUnit" % (pname, [n for n, _ in samplers]))
            continue
        if pname in RAW_PROGRAMS:
            fail("%s is in RAW_PROGRAMS but %s sets it up through the seam -- drop the entry"
                 % (pname, fns[0]))
        units = {}
        for fn in fns:
            body = setups[fn][1]
            todo = [fn] + [h for h in helpers if re.search(r"\b%s\s*\(" % h, body)]
            for f in todo:
                for _, prog, loc, name, unit in setups[f][2]:
                    units.setdefault(name, unit_value(unit, defs))
        checked += 1
        names = [n for n, _ in samplers]
        if len(set(names)) != len(names):
            fail("%s declares a sampler twice: %s" % (pname, names))
        used = {}
        for name, typ in samplers:
            if name not in units:
                if (pname, name) not in UNSET_OK:
                    fail("%s: sampler %s (%s) gets no texture unit from %s -- GL would read unit 0, "
                         "SDL_GPU a placeholder" % (pname, name, typ, "/".join(fns)))
                continue
            if (pname, name) in UNSET_OK:
                fail("UNSET_OK lists %s's %s, which %s now gives a unit -- drop the entry"
                     % (pname, name, "/".join(fns)))
            u = units[name]
            if u in used:
                fail("%s: %s and %s both read texture unit %d" % (pname, used[u], name, u))
            used[u] = name
    if checked < 20:
        fail("only %d programs checked; the scan is broken" % checked)
    # every assigned name is a sampler of some program its function sets up
    for fn, (rel, body, assigns, ids) in setups.items():
        mine = set()
        for pid in ids if ids else {i for f, v in setups.items() if re.search(r"\b%s\s*\(" % fn, v[1])
                                    for i in v[3]}:
            if pid in rows:
                for header, body_sym, blob in rows[pid][2]:
                    mine |= {n for n, _ in stage_samplers(root, body_sym, header_defines(header))}
        for _, prog, loc, name, unit in assigns:
            if name not in mine:
                fail('%s points "%s" at a unit, and none of its programs declares it' % (fn, name))
    return checked


# --------------------------------------------------------------------------
# The backend, lifted and run
# --------------------------------------------------------------------------

PRELUDE = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef uint8_t Uint8; typedef int8_t Sint8; typedef uint32_t Uint32; typedef uint16_t Uint16;
typedef int qboolean;
#define true 1
#define false 0
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum;
#define GL_COMPARE_REF_TO_TEXTURE 0x884E
#define RPROG_MAX_STAGES 2
#define RPROG_STAGE_VERTEX 0
#define RPROG_STAGE_FRAGMENT 1
#define RT_MAX_TEXTURE_UNITS 16
typedef int rprog_id_t;
typedef struct SDL_GPUShader SDL_GPUShader;
typedef struct SDL_GPUComputePipeline SDL_GPUComputePipeline;
typedef struct SDL_GPUTexture SDL_GPUTexture;
typedef struct SDL_GPUSampler SDL_GPUSampler;
typedef struct SDL_GPUSamplerCreateInfo { int x; } SDL_GPUSamplerCreateInfo;
typedef struct { SDL_GPUTexture *texture; SDL_GPUSampler *sampler; } SDL_GPUTextureSamplerBinding;
typedef unsigned int SDL_GPUTextureUsageFlags;
typedef enum {
	SDL_GPU_TEXTUREFORMAT_INVALID, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
	SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB, SDL_GPU_TEXTUREFORMAT_R8_UNORM,
	SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM,
	SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,
	SDL_GPU_TEXTUREFORMAT_R16_FLOAT, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT,
	SDL_GPU_TEXTUREFORMAT_R32_FLOAT, SDL_GPU_TEXTUREFORMAT_R16_UINT,
	SDL_GPU_TEXTUREFORMAT_R32_UINT, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT,
	SDL_GPU_TEXTUREFORMAT_R32G32B32A32_INT, SDL_GPU_TEXTUREFORMAT_D16_UNORM,
	SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
	SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT
} SDL_GPUTextureFormat;
static int notes;
static char last_note[256];
static void RG_NoteUnhandled (const char *what) { notes++; snprintf (last_note, sizeof last_note, "%s", what); }
static void Con_Printf (const char *fmt, ...) { (void) fmt; }
static void Sys_Error (const char *fmt, ...) { fprintf (stderr, "Sys_Error %s\n", fmt); exit (3); }
static size_t q_strlcpy (char *d, const char *s, size_t n) { snprintf (d, n, "%s", s); return strlen (s); }
#define q_max(a, b) ((a) > (b) ? (a) : (b))
#define q_min(a, b) ((a) < (b) ? (a) : (b))
/* the storage blocks' GL points (#340 P2e) are tools/test_rg_sources.py's */
static int RG_StoragePoint (const char *block) { (void) block; return -1; }
static int RG_StorageKind (const char *block, int *format) { (void) block; *format = -1; return 0; }
"""

HARNESS = r"""
typedef struct {
	GLuint			name;
	rprog_id_t		id;
	qboolean		compute;
	SDL_GPUShader		*vs, *fs;
	SDL_GPUComputePipeline	*cs;
	rgstage_t		stage[RPROG_MAX_STAGES];
} rgprog_t;
static rgprog_t		**rg_progs;
static int		rg_numprogs;
static const rgprog_t *RG_Program (GLuint name)
{
	if (name == 0 || (int)name > rg_numprogs)
		return NULL;
	return rg_progs[name - 1];
}
@SHADER@

/* rg_resource.c's ForDraw, with what it reaches stubbed */
typedef struct rgupload_s { int x; } rgupload_t;
@RGTEX@
static rgtex_t		rgt_tab[4];
static GLuint		rgt_units[RT_MAX_TEXTURE_UNITS][RG_TT_COUNT];
static SDL_GPUTexture	*rgt_ph[RG_SC_COUNT][RG_TT_COUNT];
static SDL_GPUTexture	*rgt_ph_shadow;
static SDL_GPUSampler	*rgt_ph_sampler, *rgt_ph_shadow_sampler;
static char		fake[64];
static rgtex_t *RGT_Get (GLuint name) { return (name && name < 4 && rgt_tab[name].alive) ? &rgt_tab[name] : NULL; }
static void RGT_Realize (rgtex_t *t) { if (!t->tex) t->tex = (SDL_GPUTexture *) &fake[40]; }
static SDL_GPUSampler *RGT_Sampler (rgtex_t *t) { (void) t; return (SDL_GPUSampler *) &fake[50]; }
static unsigned int RG_Frame_Segment (void) { return 7; }
@RES@

static Uint32 *load (const char *path, size_t *n)
{
	FILE *f = fopen (path, "rb");
	long sz;
	Uint32 *w;
	if (!f) exit (4);
	fseek (f, 0, SEEK_END); sz = ftell (f); fseek (f, 0, SEEK_SET);
	w = malloc (sz);
	if (fread (w, 1, sz, f) != (size_t) sz) exit (4);
	fclose (f);
	*n = sz / 4;
	return w;
}

int main (int argc, char **argv)
{
	static rgprog_t	prog;
	rgprog_t	*pp = &prog;
	size_t		n;
	int		i, s, c;
	Uint32		*w;

	/* 1. reflection: argv[1] a fragment stage, argv[2] a vertex stage */
	w = load (argv[1], &n);
	if (!RG_Reflect (w, n, 2, 3, &prog.stage[RPROG_STAGE_FRAGMENT], "frag")) { puts ("REFLECT-FAIL frag"); return 1; }
	w = load (argv[2], &n);
	if (!RG_Reflect (w, n, 0, 1, &prog.stage[RPROG_STAGE_VERTEX], "vert")) { puts ("REFLECT-FAIL vert"); return 1; }
	printf ("NOTES-AT-REFLECT %d\n", notes);
	for (s = 0; s < 2; s++)
		for (i = 0; i < prog.stage[s].num_samplers; i++)
		{
			const rgsampler_t *x = &prog.stage[s].samplers[i];
			printf ("S %d %s binding=%d type=%d class=%d shadow=%d ms=%d unit=%d\n", s, x->name,
				x->binding, x->type, x->sclass, x->shadow, x->ms, x->unit);
		}

	/* 2. the seam's backend half */
	rg_progs = &pp;
	rg_numprogs = 1;
	printf ("LOC u_lightgrid %d\n", RG_ProgramSampler (1, "u_lightgrid"));
	printf ("LOC u_vertsamp %d\n", RG_ProgramSampler (1, "u_vertsamp"));
	printf ("LOC u_missing %d\n", RG_ProgramSampler (1, "u_missing"));
	printf ("LOC badprog %d\n", RG_ProgramSampler (2, "u_lightgrid"));
	RG_ProgramSamplerUnit (1, "u_lightgrid", 5);
	RG_ProgramSamplerUnit (1, "u_both", 9);
	notes = 0;
	RG_ProgramSamplerUnit (1, "u_texture3", 16);
	printf ("NOTES-AT-BADUNIT %d\n", notes);
	for (s = 0; s < 2; s++)
		for (i = 0; i < prog.stage[s].num_samplers; i++)
			printf ("U %d %s unit=%d\n", s, prog.stage[s].samplers[i].name, prog.stage[s].samplers[i].unit);

	/* 3. placeholders and what a real texture may be bound to */
	for (c = 0; c < RG_SC_COUNT; c++)
		for (i = 0; i < RG_TT_COUNT; i++)
			rgt_ph[c][i] = (SDL_GPUTexture *) &fake[c * RG_TT_COUNT + i];
	rgt_ph_shadow = (SDL_GPUTexture *) &fake[30];
	rgt_ph_sampler = (SDL_GPUSampler *) &fake[31];
	rgt_ph_shadow_sampler = (SDL_GPUSampler *) &fake[32];
	for (c = 0; c < RG_SC_COUNT; c++)
		for (i = 0; i < RG_TT_COUNT; i++)
		{
			SDL_GPUTextureSamplerBinding b;
			qboolean ok = RG_Res_ForDraw (RG_UNIT_NONE, (rg_textype_t) i, (rg_sampclass_t) c, false, &b);
			printf ("PH class=%d type=%d ok=%d tex=%d sampler=%d\n", c, i, ok,
				(int) ((char *) b.texture - fake), (int) ((char *) b.sampler - fake));
		}
	for (i = 0; i < RG_TT_COUNT; i++)
	{
		SDL_GPUTextureSamplerBinding b;
		qboolean ok = RG_Res_ForDraw (RG_UNIT_NONE, (rg_textype_t) i, RG_SC_FLOAT, true, &b);
		printf ("PHSHADOW type=%d ok=%d tex=%d sampler=%d\n", i, ok,
			ok ? (int) ((char *) b.texture - fake) : -1, ok ? (int) ((char *) b.sampler - fake) : -1);
	}
	/* a real texture on unit 5: RGBA32UI 3D, then a float 3D, then a depth cube array */
	{
		static const struct { SDL_GPUTextureFormat fmt; int type; int unit; int sclass; int shadow; int cmp; } k[] = {
			{ SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT, RG_TT_3D, 5, RG_SC_UINT, 0, 0 },
			{ SDL_GPU_TEXTUREFORMAT_R32G32B32A32_UINT, RG_TT_3D, 5, RG_SC_FLOAT, 0, 0 },
			{ SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, RG_TT_3D, 5, RG_SC_UINT, 0, 0 },
			{ SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, RG_TT_3D, 5, RG_SC_FLOAT, 0, 0 },
			{ SDL_GPU_TEXTUREFORMAT_D16_UNORM, RG_TT_CUBE_ARRAY, 9, RG_SC_FLOAT, 1, 1 },
			{ SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, RG_TT_CUBE_ARRAY, 9, RG_SC_FLOAT, 1, 1 },
			{ SDL_GPU_TEXTUREFORMAT_D16_UNORM, RG_TT_CUBE_ARRAY, 9, RG_SC_FLOAT, 0, 0 },
			{ SDL_GPU_TEXTUREFORMAT_D16_UNORM, RG_TT_CUBE_ARRAY, 9, RG_SC_FLOAT, 1, 0 },
			{ SDL_GPU_TEXTUREFORMAT_D16_UNORM, RG_TT_CUBE_ARRAY, 9, RG_SC_FLOAT, 0, 1 },
		};
		for (i = 0; i < (int) (sizeof k / sizeof k[0]); i++)
		{
			SDL_GPUTextureSamplerBinding b;
			qboolean ok;

			memset (rgt_tab, 0, sizeof rgt_tab);
			memset (rgt_units, 0, sizeof rgt_units);
			rgt_tab[1].alive = rgt_tab[1].defined = true;
			rgt_tab[1].type = (rg_textype_t) k[i].type;
			rgt_tab[1].fmt = k[i].fmt;
			rgt_tab[1].compare_mode = k[i].cmp ? GL_COMPARE_REF_TO_TEXTURE : 0;
			rgt_units[k[i].unit][k[i].type] = 1;
			ok = RG_Res_ForDraw (k[i].unit, (rg_textype_t) k[i].type, (rg_sampclass_t) k[i].sclass,
					     k[i].shadow, &b);
			printf ("REAL %d ok=%d tex=%d\n", i, ok, (int) ((char *) b.texture - fake));
		}
	}
	return 0;
}
"""


class Spv:
    """A minimal SPIR-V module: samplers only, enough for RG_Reflect."""

    def __init__(self):
        self.words, self.next = [], 1
        self.f32 = self.new()
        self.u32 = self.new()
        self.i32 = self.new()
        self.types = [(22, [self.f32, 32]), (21, [self.u32, 32, 0]), (21, [self.i32, 32, 1])]
        self.decls = []

    def new(self):
        self.next += 1
        return self.next - 1

    @staticmethod
    def string(s):
        b = s.encode() + b"\0"
        b += b"\0" * (-len(b) % 4)
        return list(struct.unpack("<%dI" % (len(b) // 4), b))

    def sampler(self, name, dset, binding, sampled, dim, depth=0, arrayed=0, ms=0):
        img, simg, ptr, var = self.new(), self.new(), self.new(), self.new()
        st = {"f": self.f32, "u": self.u32, "i": self.i32}[sampled]
        self.types += [(25, [img, st, dim, depth, arrayed, ms, 1, 0]), (27, [simg, img]),
                       (32, [ptr, 0, simg]), (59, [ptr, var, 0])]
        self.decls += [(5, [var] + self.string(name)), (71, [var, 34, dset]), (71, [var, 33, binding])]

    def blob(self):
        out = [0x07230203, 0x00010000, 0, self.next, 0]
        for op, ops in self.decls + self.types:
            out += [((len(ops) + 1) << 16) | op] + ops
        return struct.pack("<%dI" % len(out), *out)


DIM2D, DIM3D, DIMCUBE = 1, 2, 3


def check_backend(root, verbose):
    rgi = strip_c((root / RG_INTERNAL_H).read_text())
    rgs = strip_c((root / RG_SHADER_C).read_text())
    rgr = strip_c((root / RG_RES_C).read_text())
    rgf = strip_c((root / RG_FRAME_C).read_text())

    parts = [lift(rgi, r"^typedef enum \{\s*RG_TT_2D,", "rg_textype_t"),
             lift(rgi, r"^typedef enum \{\s*RG_SC_FLOAT,", "rg_sampclass_t")]
    for d in ("RG_UNIT_NONE", "RG_MAX_STAGE_SAMPLERS", "RG_MAX_STAGE_UBOS", "RG_MAX_STAGE_SSBOS",
              "RG_SSBO_STANDIN_BYTES"):
        m = re.search(r"^#define %s\s+\S+" % d, rgi, re.M)
        if not m:
            fail("rg_internal.h: no %s" % d)
        else:
            parts.append(m.group(0))
    for t in ("rgsampler_t", "rgubo_t", "rgssbo_t", "rgstage_t"):
        m = re.search(r"typedef struct \{[^{}]*\} %s;" % t, rgi)
        if not m:
            fail("rg_internal.h: cannot lift %s" % t)
        else:
            parts.append(m.group(0))
    prelude = PRELUDE + "\n".join(parts) + "\n"
    shader = "\n".join([
        lift(rgs, r"^enum \{\s*SPV_OpName", "rg_shader.c's SPIR-V enum"),
        re.search(r"typedef struct \{[^{}]*\} spvid_t;", rgs).group(0) if re.search(
            r"typedef struct \{[^{}]*\} spvid_t;", rgs) else "",
        lift(rgs, r"^static rg_textype_t RG_ImageType \(", "RG_ImageType"),
        lift(rgs, r"^static qboolean RG_Reflect \(", "RG_Reflect"),
        lift(rgs, r"^GLint RG_ProgramSampler \(", "RG_ProgramSampler"),
        lift(rgs, r"^void RG_ProgramSamplerUnit \(", "RG_ProgramSamplerUnit"),
    ])
    rgtex = re.search(r"typedef struct \{\s*qboolean\s+alive;.*?\} rgtex_t;", rgr, re.S)
    res = "\n".join([
        lift(rgr, r"^static rg_sampclass_t RGT_FormatClass \(", "RGT_FormatClass"),
        lift(rgr, r"^static qboolean RGT_IsDepth \(", "RGT_IsDepth"),
        lift(rgr, r"^qboolean RG_Res_ForDraw \(", "RG_Res_ForDraw"),
    ])
    if not rgtex:
        fail("rg_resource.c: cannot lift rgtex_t")
    if errors:
        return
    src = prelude + HARNESS.replace("@SHADER@", shader).replace("@RGTEX@", rgtex.group(0)) \
        .replace("@RES@", res)

    frag = Spv()
    frag.sampler("u_texture0", 2, 0, "f", DIM2D)
    frag.sampler("u_texture3", 2, 1, "f", DIM2D)
    frag.sampler("u_lightgrid", 2, 2, "u", DIM3D)
    frag.sampler("u_dlshadowmap", 2, 3, "f", DIMCUBE, depth=1, arrayed=1)
    frag.sampler("u_shadowdbg", 2, 4, "f", DIMCUBE, arrayed=1)
    frag.sampler("u_cube", 2, 5, "f", DIMCUBE)
    frag.sampler("u_layers", 2, 6, "f", DIM2D, arrayed=1)
    frag.sampler("u_ints", 2, 7, "i", DIM2D)
    frag.sampler("u_mboit_b0_ms", 2, 8, "f", DIM2D, ms=1)
    frag.sampler("u_both", 2, 9, "f", DIM2D)
    vert = Spv()
    vert.sampler("u_both", 0, 0, "f", DIM2D)
    vert.sampler("u_vertsamp", 0, 1, "f", DIM3D)
    vert.sampler("u_texture3", 0, 2, "f", DIM2D)

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        (td / "h.c").write_text(src)
        (td / "f.spv").write_bytes(frag.blob())
        (td / "v.spv").write_bytes(vert.blob())
        cc = os.environ.get("CC", "cc")
        r = subprocess.run([cc, "-std=gnu99", "-w", "-o", str(td / "h"), str(td / "h.c")],
                           capture_output=True, text=True)
        if r.returncode:
            fail("the lifted backend does not compile:\n" + r.stderr[-3000:])
            return
        r = subprocess.run([str(td / "h"), str(td / "f.spv"), str(td / "v.spv")],
                           capture_output=True, text=True)
        out = r.stdout
    if verbose:
        print(out)
    if r.returncode:
        fail("the lifted backend exits %d:\n%s" % (r.returncode, out[-2000:]))
        return

    def enum_value(name, enum_text):
        names = re.findall(r"\b(RG_\w+)\b", enum_text)
        return names.index(name)
    tt = parts[0]
    sc = parts[1]
    T = {n: enum_value("RG_TT_" + n, tt) for n in ("2D", "3D", "CUBE", "2D_ARRAY", "CUBE_ARRAY", "COUNT")}
    C = {n: enum_value("RG_SC_" + n, sc) for n in ("FLOAT", "SINT", "UINT", "COUNT")}
    none = 0xFF
    want = {
        # name: (type, class, shadow, ms, unit at reflection, unit after the seam)
        (1, "u_texture0"): (T["2D"], C["FLOAT"], 0, 0, 0, 0),
        (1, "u_texture3"): (T["2D"], C["FLOAT"], 0, 0, 3, 3),
        (1, "u_lightgrid"): (T["3D"], C["UINT"], 0, 0, none, 5),
        (1, "u_dlshadowmap"): (T["CUBE_ARRAY"], C["FLOAT"], 1, 0, none, none),
        (1, "u_shadowdbg"): (T["CUBE_ARRAY"], C["FLOAT"], 0, 0, none, none),
        (1, "u_cube"): (T["CUBE"], C["FLOAT"], 0, 0, none, none),
        (1, "u_layers"): (T["2D_ARRAY"], C["FLOAT"], 0, 0, none, none),
        (1, "u_ints"): (T["2D"], C["SINT"], 0, 0, none, none),
        (1, "u_mboit_b0_ms"): (T["2D"], C["FLOAT"], 0, 1, none, none),
        (1, "u_both"): (T["2D"], C["FLOAT"], 0, 0, none, 9),
        (0, "u_both"): (T["2D"], C["FLOAT"], 0, 0, none, 9),
        (0, "u_vertsamp"): (T["3D"], C["FLOAT"], 0, 0, none, none),
        (0, "u_texture3"): (T["2D"], C["FLOAT"], 0, 0, 3, 3),
    }
    got = {}
    for m in re.finditer(r"^S (\d) (\w+) binding=(\d+) type=(\d+) class=(\d+) shadow=(\d+) ms=(\d+) unit=(\d+)$",
                         out, re.M):
        got[(int(m.group(1)), m.group(2))] = tuple(int(m.group(k)) for k in (4, 5, 6, 7, 8))
    after = {(int(m.group(1)), m.group(2)): int(m.group(3))
             for m in re.finditer(r"^U (\d) (\w+) unit=(\d+)$", out, re.M)}
    for key, w in want.items():
        g = got.get(key)
        if g is None:
            fail("RG_Reflect lost sampler %s of stage %d" % (key[1], key[0]))
            continue
        if g[:4] != w[:4]:
            fail("RG_Reflect reads %s as type/class/shadow/ms %s, want %s" % (key[1], g[:4], w[:4]))
        if g[4] != w[4]:
            fail("RG_Reflect gives %s (stage %d) unit %d at reflection, want %s" % (
                key[1], key[0], g[4], "none (RG_UNIT_NONE)" if w[4] == none else w[4]))
        if after.get(key) != w[5]:
            fail("after RG_ProgramSamplerUnit, %s (stage %d) reads unit %s, want %s" % (
                key[1], key[0], after.get(key), "none" if w[5] == none else w[5]))
    if len(got) != len(want):
        fail("RG_Reflect found %d samplers, the modules declare %d" % (len(got), len(want)))
    if "NOTES-AT-REFLECT 0" not in out:
        fail("RG_Reflect notes an unhandled request: the unmapped-sampler note belongs at draw time")
    if "NOTES-AT-BADUNIT 1" not in out:
        fail("RG_ProgramSamplerUnit must refuse (and count) a unit above 15")
    for name, v in (("u_lightgrid", "2"), ("u_vertsamp", "1"), ("u_missing", "-1"), ("badprog", "-1")):
        if "LOC %s %s\n" % (name, v) not in out:
            fail("RG_ProgramSampler (%s) must be %s" % (name, v))

    # placeholders: exactly the sampler's kind
    for m in re.finditer(r"^PH class=(\d+) type=(\d+) ok=(\d+) tex=(-?\d+) sampler=(-?\d+)$", out, re.M):
        c, t, ok, tex, smp = map(int, m.groups())
        if not ok or tex != c * T["COUNT"] + t or smp != 31:
            fail("RG_Res_ForDraw binds class %d type %d to placeholder %d / sampler %d, want %d / 31"
                 % (c, t, tex, smp, c * T["COUNT"] + t))
    for m in re.finditer(r"^PHSHADOW type=(\d+) ok=(\d+) tex=(-?\d+) sampler=(-?\d+)$", out, re.M):
        t, ok, tex, smp = map(int, m.groups())
        if t == T["CUBE_ARRAY"]:
            if not ok or tex != 30 or smp != 32:
                fail("a samplerCubeArrayShadow must get the depth cube array and the compare sampler")
        elif ok:
            fail("a shadow sampler of type %d has no placeholder; RG_Res_ForDraw must refuse it, "
                 "not bind a colour texture" % t)
    if len(re.findall(r"^PH ", out, re.M)) != C["COUNT"] * T["COUNT"]:
        fail("placeholder probe incomplete")
    real = {int(m.group(1)): (int(m.group(2)), int(m.group(3)))
            for m in re.finditer(r"^REAL (\d+) ok=(\d+) tex=(-?\d+)$", out, re.M)}
    # 7: a depth texture with no compare mode on a shadow sampler, 8: one with
    # compare mode on a plain sampler -- each a sampler the shader cannot use
    expect_real = {0: 40, 1: C["FLOAT"] * T["COUNT"] + T["3D"], 2: C["UINT"] * T["COUNT"] + T["3D"],
                   3: 40, 4: 40, 5: 30, 6: 40, 7: 30, 8: C["FLOAT"] * T["COUNT"] + T["CUBE_ARRAY"]}
    for i, tex in expect_real.items():
        if real.get(i) != (1, tex):
            fail("RG_Res_ForDraw case %d binds %s, want texture %d (40 the real one)" % (i, real.get(i), tex))

    # the placeholder formats are the classes they stand for
    cp = lift(rgr, r"^static void RGT_CreatePlaceholders \(", "RGT_CreatePlaceholders")
    fm = re.search(r"fmts\[RG_SC_COUNT\]\s*=\s*\{([^}]*)\}", cp)
    want_fmt = {"FLOAT": r"_UNORM$", "SINT": r"_INT$", "UINT": r"_UINT$"}
    if not fm:
        fail("RGT_CreatePlaceholders: no fmts[RG_SC_COUNT] table")
    else:
        fl = [x.strip() for x in fm.group(1).split(",") if x.strip()]
        for cname, rx in want_fmt.items():
            i = C[cname]
            if i >= len(fl) or not re.search(rx, fl[i]) or "R32G32B32A32" not in fl[i] and cname != "FLOAT":
                fail("placeholder format for class %s is %s" % (cname, fl[i] if i < len(fl) else None))
    made = [split_args(cp[m.end():match_close(cp, m.end() - 1, "(", ")") - 1])
            for m in re.finditer(r"rgt_ph_shadow = RGT_MakePlaceholder \(", cp)]
    if not made or "enable_compare = true" not in cp or any(
            len(a) != 3 or a[0] != "RG_TT_CUBE_ARRAY"
            or not re.fullmatch(r"SDL_GPU_TEXTUREFORMAT_D(16_UNORM|32_FLOAT)", a[1])
            or "SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET" not in a[2] for a in made):
        fail("the shadow placeholder must be a depth cube array (every format tried a depth one, created "
             "as a depth target so it can be cleared) read through a compare sampler")

    # the placeholders' texels: RGBA8 white, RGBA32 zero, each class 16-byte
    # aligned (Vulkan wants a copy offset a multiple of the texel size), staged
    # after the stand-in in the copy pass, and the depth one cleared after it
    tx = lift(rgr, r"^static int RGT_PhTexel \(", "RGT_PhTexel")
    if "return (sclass == RG_SC_FLOAT) ? 4 : 16;" not in tx:
        fail("RGT_PhTexel must be 4 bytes for the RGBA8 placeholders and 16 for the RGBA32 int / uint ones")
    pb = lift(rgr, r"^static Uint32 RGT_PhBytes \(", "RGT_PhBytes")
    if "return (n + 15) & ~15u;" not in pb or "n += RGT_PhLayers (i) * RGT_PhTexel (sclass);" not in pb:
        fail("RGT_PhBytes must count one texel per layer of each dimensionality and pad the class to 16 bytes")
    su = lift(rgr, r"^void RG_Res_StageUploads \(", "RG_Res_StageUploads")
    if not re.search(r"memset \(dst, 0, RGT_PhStaged \(\)\);\s*memset \(dst, 255, RGT_PhBytes \(RG_SC_FLOAT\)\);", su):
        fail("RG_Res_StageUploads must stage zeros for every placeholder and white for the float ones first")
    iu = lift(rgr, r"^void RG_Res_IssueUploads \(", "RG_Res_IssueUploads")
    if not re.search(r"for \(c = 0; c < RG_SC_COUNT; start \+= RGT_PhBytes \(c\), c\+\+\)\s*\{\s*off = start;", iu) \
            or "off += RGT_PhTexel (c)" not in iu:
        fail("RG_Res_IssueUploads must start each class at its aligned offset and step a texel per layer")
    cps0 = lift(rgf, r"^static qboolean RGF_CopyPass \(", "RGF_CopyPass")
    if "RG_Res_StageUploads (map + stream + quad + ssbo, stream + quad + ssbo);" not in cps0:
        fail("RGF_CopyPass must stage the textures after the stand-in's zeros, not over them")
    es = lift(rgf, r"^static void RGF_EndSegment \(", "RGF_EndSegment")
    if not re.search(r"if \(!RGF_CopyPass \(\)\)[^;]*;[^\n]*\n\s*RG_Res_ClearPlaceholders \(rgf_cmd\);", es):
        fail("RGF_EndSegment must clear the depth placeholders after the copy pass, before the scene's pass")

    # draw time: the unmapped note, and every storage-buffer slot bound
    rs = lift(rgf, r"^static qboolean RGF_RecordStage \(", "RGF_RecordStage")
    if not re.search(r"if \(s->unit == RG_UNIT_NONE\)\s*RG_NoteUnhandled \(\"sampler with no GL unit mapping", rs):
        fail("RGF_RecordStage must note a sampler with no GL unit mapping, at draw time")
    if not re.search(r"if \(!RG_Res_ForDraw \(s->unit, \(rg_textype_t\) s->type, \(rg_sampclass_t\) s->sclass,"
                     r"\s*s->shadow, &out\[s->binding\]\)\)\s*return false;", rs):
        fail("RGF_RecordStage must bind each sampler by its unit, type, class and shadow flag, and drop "
             "the draw when there is nothing to bind")
    if not re.search(r"d->nvssbo = \(Uint8\) st->num_ssbos;", rs) or \
            not re.search(r"d->nfssbo = \(Uint8\) st->num_ssbos;", rs):
        fail("RGF_RecordStage must record each stage's read-only storage-buffer count")
    rp = lift(rgf, r"^static void RGF_Replay \(", "RGF_Replay")
    for st in ("Vertex", "Fragment"):
        f = st[0].lower()
        if not re.search(r"if \(d->n%sssbo\)\s*SDL_BindGPU%sStorageBuffers \(pass, 0, d->%sssbo, d->n%sssbo\);"
                         % (f, st, f, f), rp):
            fail("RGF_Replay must bind every one of the %s stage's storage-buffer slots, each the "
                 "buffer or stand-in RGF_RecordStage put there (#340 P2e)" % st.lower())
    if "slots[sb->slot] = rgf_ssbo;" not in rs:
        fail("RGF_RecordStage must bind the stand-in to a storage slot with nothing at its GL point")
    init = lift(rgf, r"^void RG_Frame_Init \(", "RG_Frame_Init")
    if not re.search(r"ci\.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;\s*ci\.size = RG_SSBO_STANDIN_BYTES;", init):
        fail("RG_Frame_Init must create the RG_SSBO_STANDIN_BYTES stand-in")
    cps = lift(rgf, r"^static qboolean RGF_CopyPass \(", "RGF_CopyPass")
    if "memset (map + stream + quad, 0, ssbo);" not in cps or "rgf_ssbo_uploaded = true;" not in cps:
        fail("RGF_CopyPass must upload the stand-in's zeros once")
    ck = lift(rgs, r"^static qboolean RG_CheckStage \(", "RG_CheckStage")
    if "if (out->num_ssbos != refl->ro_storage_buffers)" not in ck:
        fail("RG_CheckStage must hold each stage's reflected storage blocks to its reflection row's count")


def main(argv=None):
    global ROOT
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    root = args.root.resolve()

    defs = constants(root)
    code = strip_c((root / SHADER_C).read_text())
    check_seam(code)
    check_routing(root)
    rows = program_rows(code)
    setups, allfns = setup_functions(root, rows)
    check_call_sites(setups, allfns, defs)
    n = check_programs(root, rows, setups, defs)
    check_backend(root, args.verbose)
    if args.verbose:
        for fn, (rel, body, assigns, ids) in sorted(setups.items()):
            print("%-26s %-22s %s" % (fn, ",".join(sorted(i[6:].lower() for i in ids)) or "(helper)",
                                     " ".join("%s=%s" % (a[3], a[4]) for a in assigns)))
    if errors:
        for e in errors:
            print("FAIL: " + e, file=sys.stderr)
        print("%d sampler-unit problem(s)" % len(errors), file=sys.stderr)
        return 1
    print("sampler units OK: %d programs through the seam, %d set-up functions, the backend's "
          "reflection, unit seam and placeholders run" % (n, len(setups)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
