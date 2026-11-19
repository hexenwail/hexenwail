#!/usr/bin/env python3
"""engine/shaders/variants.txt enumerates every stage + #define set the engine
can build at run time (GitHub #340, phase P-1b).

GL compiles some files several times, each with a different header chunk
("#define NOPERSP 1", "#define QUALITY %d", "#define MODE %d", the froxel-grid
sizes).  SDL_GPU cannot compile GLSL at run time, so every one of those
programs needs an offline blob, and EmbedShaders.cmake writes a copy per
`variant` line of variants.txt for the SPIR-V and cross-compile gates.  This
lint proves the manifest is the set the C can actually ask for, by reading
the C:

  * Every program is a row of gl_shader.c's rprog_defs table (one per
    rprog_id_t, #340 P1b), and RP_CompileStage, which hands a row's two
    chunks to glShaderSource, is the only compile there is.  Each row's
    header chunk is evaluated as the compiler would: string literals and
    macros concatenated (GLSL_STR stringizes), integer + - * with C
    precedence.  Anything else -- a cvar, a function result -- is a
    failure: a variant that depends on a run-time value has no finite set
    to compile.  (Until P1b the headers were built at the call sites, and
    this walked every call chain into four glShaderSource callers to
    evaluate them; the table made that walk unnecessary.)
  * The resulting #define lines, per stage, must match an offline copy: the
    plain stage, its OIT twin, or a `variant` line.  `const` defines are
    checked separately: the C must pass the manifest's value, and every
    shader that defaults the name must default it to the same value, which
    is what makes the plain offline compile the same program.
  * Nothing escapes the table: every C string literal holding "#define"
    under engine/h2shared and engine/hexen2 must have been consumed by a
    row, every shaders_gen.h stage symbol named in the C must be a row's
    body, glShaderSource may be called only inside RP_CompileStage, and
    RP_CompileStage only by R_Program_Create -- not through a macro, a
    function pointer or a file the parser could not read.
    tools/test_program_routing.py holds the rest: no other file creates,
    compiles or links a program, and every row has its offline blobs.
  * Every `variant` and `const` line, and every OIT twin EmbedShaders
    writes, is needed by some program; stale ones fail.
  * CompileSpirv.cmake does not pass --invert-y: glslang implements it in
    the HLSL front end only and ignores it for GLSL, so it would only look
    like the SDL_GPU Y flip.

Desktop GL only: the C is read with USE_GLES, H2W, SERVERONLY, WEBSOFT and
__EMSCRIPTEN__ undefined, GLQUAKE and GL_DLSYM defined; any other
conditional keeps both arms.  The GLES/WebGL2 tier never runs SDL_GPU.

Usage:  test_shader_variants.py [--root DIR] [--vk-dir VK_DIR] [-v]

--vk-dir names EmbedShaders.cmake's output (<build>/engine/generated/vk) and
also checks that it holds exactly the expected copies, each variant with its
manifest #defines after the #version line; that part runs in
shaders-spirv.yml.  Stock python3 only.
"""
from pathlib import Path
import argparse
import re
import sys

SCAN_DIRS = ("engine/h2shared", "engine/hexen2")
# The WEBSOFT canvas presenter (CMake builds it only for the software web
# renderer); its blit is in engine/shaders/glonly and never reaches SDL_GPU.
EXCLUDED = {"engine/h2shared/web_canvas_gl2.c"}

# The desktop GL client, as the SDL_GPU backend will be built.
DEFINED = {"GLQUAKE", "GL_DLSYM"}
UNDEFINED = {"USE_GLES", "H2W", "SERVERONLY", "WEBSOFT", "__EMSCRIPTEN__"}

# Every program the renderer builds is a row of this table, and this is the
# one function that hands source to glShaderSource: a row's two chunks.
# tools/test_program_routing.py holds the rest of that (no other file
# creates or links a program, every R_Program_Create argument is an id).
TABLE_FILE = "engine/h2shared/gl_shader.c"
TABLE_NAME = "rprog_defs"
LEAF = "RP_CompileStage"
CREATOR = "R_Program_Create"
# Names glslang reserves or predefines; a variant may not set them.
RESERVED = re.compile(r"^(VULKAN|GL_\w+|__\w+)$")

DEFINE_RE = re.compile(r"#\s*define\b")

KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do", "else",
            "case", "goto", "defined"}


class Fail(Exception):
    pass


class Unresolved(Exception):
    pass


# --------------------------------------------------------------------------
# Lexing and the bits of the preprocessor this needs
# --------------------------------------------------------------------------

class Tok:
    __slots__ = ("kind", "val", "file", "line", "pos")

    def __init__(self, kind, val, file, line, pos=-1):
        self.kind, self.val, self.file, self.line, self.pos = kind, val, file, line, pos

    def __repr__(self):
        return "%s:%r" % (self.kind, self.val)


ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f",
           "v": "\v", "\\": "\\", "'": "'", '"': '"', "?": "?", "0": "\0"}


def decode(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            n = body[i + 1]
            if n == "x":
                m = re.match(r"[0-9A-Fa-f]+", body[i + 2:])
                out.append(chr(int(m.group(0), 16)) if m else "x")
                i += 2 + (len(m.group(0)) if m else 0)
                continue
            out.append(ESCAPES.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def strip_comments(text):
    """Comments to spaces, newlines kept, strings and chars left alone."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
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
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


TOKEN_RE = re.compile(r'''
    (?P<str>(?:u8|[LuU])?"(?:[^"\\\n]|\\.)*")
  | (?P<chr>'(?:[^'\\\n]|\\.)*')
  | (?P<num>\.?\d(?:[\w.]|[eEpP][+-])*)
  | (?P<id>[A-Za-z_]\w*)
  | (?P<p>\#\#|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^~!<>=?:;,.(){}\[\]\#])
  | (?P<ws>\s+)
  | (?P<other>.)
''', re.X)


def tokenize(text, file, line):
    toks = []
    for m in TOKEN_RE.finditer(text):
        kind = m.lastgroup
        v = m.group(kind)
        if kind == "ws":
            line += v.count("\n")
            continue
        if kind == "str":
            toks.append(Tok("str", decode(v[v.index('"') + 1:-1]), file, line))
        elif kind == "chr":
            toks.append(Tok("chr", v, file, line))
        elif kind == "num":
            toks.append(Tok("num", v, file, line))
        elif kind == "id":
            toks.append(Tok("id", v, file, line))
        else:
            toks.append(Tok("p", v, file, line))
    return toks


class Macro:
    def __init__(self, name, params, body, file, line):
        self.name, self.params, self.body, self.file, self.line = name, params, body, file, line

    def key(self):
        return (tuple(self.params) if self.params is not None else None,
                tuple((t.kind, t.val) for t in self.body))


def cond_eval(toks):
    """#if expression -> True / False / None (unknown)."""
    pos = [0]

    def peek():
        return toks[pos[0]].val if pos[0] < len(toks) else None

    def take():
        t = toks[pos[0]]
        pos[0] += 1
        return t

    def primary():
        t = take()
        if t.val == "(":
            v = expr()
            if peek() == ")":
                take()
            return v
        if t.val == "!":
            v = primary()
            return None if v is None else (0 if v else 1)
        if t.val == "defined":
            paren = peek() == "("
            if paren:
                take()
            name = take().val
            if paren and peek() == ")":
                take()
            return 1 if name in DEFINED else 0 if name in UNDEFINED else None
        if t.kind == "num":
            try:
                return int(t.val.rstrip("uUlL"), 0)
            except ValueError:
                return None
        return None

    def binop(sub, ops):
        def f():
            v = sub()
            while peek() in ops:
                op = take().val
                w = sub()
                if op == "&&":
                    v = 0 if (v == 0 or w == 0) else (None if v is None or w is None else 1)
                elif op == "||":
                    v = 1 if (v not in (0, None) or w not in (0, None)) else \
                        (None if v is None or w is None else 0)
                elif v is None or w is None:
                    v = None
                else:
                    v = int({"==": v == w, "!=": v != w, "<": v < w, ">": v > w,
                             "<=": v <= w, ">=": v >= w}[op])
            return v
        return f

    cmp_ = binop(primary, ("==", "!=", "<", ">", "<=", ">="))
    and_ = binop(cmp_, ("&&",))
    expr = binop(and_, ("||",))
    try:
        v = expr()
    except IndexError:
        return None
    return None if v is None else bool(v)


class Source:
    """One .c or .h: active code tokens (positions are indices) and macros."""

    def __init__(self, path, rel):
        self.rel = rel
        self.def_ids = set()       # id() of function-definition name tokens
        self.macros = {}
        self.toks = []
        text = strip_comments(path.read_text(errors="replace"))
        lines = text.split("\n")
        stack = []          # (parent_active, taken, active)
        active = True
        i = 0
        while i < len(lines):
            start = i + 1
            line = lines[i]
            while line.endswith("\\") and i + 1 < len(lines):
                i += 1
                line = line[:-1] + " " + lines[i]
            i += 1
            m = re.match(r"\s*#\s*(\w+)(.*)$", line)
            if not m:
                if active:
                    self.toks += tokenize(line, rel, start)
                continue
            d, rest = m.group(1), m.group(2)
            if d in ("if", "ifdef", "ifndef"):
                if d == "if":
                    v = cond_eval(tokenize(rest, rel, start))
                else:
                    name = rest.split()[0] if rest.split() else ""
                    v = True if name in DEFINED else False if name in UNDEFINED else None
                    if d == "ifndef" and v is not None:
                        v = not v
                stack.append([active, v, None])
                active = active and v is not False
                stack[-1][2] = active
            elif d == "elif":
                parent, taken, _ = stack[-1]
                if taken is True:
                    active = False
                else:
                    v = cond_eval(tokenize(rest, rel, start))
                    active = parent and v is not False
                    stack[-1][1] = True if v is True else (None if (taken is None or v is None) else False)
            elif d == "else":
                parent, taken, _ = stack[-1]
                active = parent and taken is not True
            elif d == "endif":
                active = stack.pop()[0] if stack else True
            elif d == "define" and active:
                dm = re.match(r"\s*([A-Za-z_]\w*)(\([^)]*\))?(.*)$", rest)
                if dm:
                    params = None
                    if dm.group(2) is not None:
                        params = [p.strip() for p in dm.group(2)[1:-1].split(",") if p.strip()]
                    body = tokenize(dm.group(3), rel, start)
                    self.macros.setdefault(dm.group(1), []).append(
                        Macro(dm.group(1), params, body, rel, start))
        for k, t in enumerate(self.toks):
            t.pos = k


# --------------------------------------------------------------------------
# Functions, calls, enums
# --------------------------------------------------------------------------

def matching(toks, i, open_, close):
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].val == open_:
            depth += 1
        elif toks[j].val == close:
            depth -= 1
            if depth == 0:
                return j
    raise Fail("%s:%d: unbalanced %s" % (toks[i].file, toks[i].line, open_))


def split_args(toks):
    args, cur, depth = [], [], 0
    for t in toks:
        if t.val in "([{" and t.kind == "p":
            depth += 1
        elif t.val in ")]}" and t.kind == "p":
            depth -= 1
        if t.val == "," and t.kind == "p" and depth == 0:
            args.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur or args:
        args.append(cur)
    return args


class Func:
    def __init__(self, src, name, static, params, body_start, body_end, ret=()):
        self.src, self.name, self.static, self.ret = src, name, static, [x.val for x in ret]
        self.params = params            # [(name, [type tokens])]
        self.body = (body_start, body_end)

    def toks(self):
        return self.src.toks[self.body[0]:self.body[1] + 1]

    def where(self):
        t = self.src.toks[self.body[0]]
        return "%s:%d" % (t.file, t.line)


def parse_functions(src, enums):
    toks, funcs = src.toks, []
    i, n, stmt_start = 0, len(toks), 0
    while i < n:
        t = toks[i]
        if t.val in (";", "}") and t.kind == "p":
            stmt_start = i + 1
            i += 1
            continue
        if t.val == "{" and t.kind == "p":
            j = matching(toks, i, "{", "}")
            head = toks[stmt_start:i]
            # typedef enum [tag] { ... } Name ;
            if head and head[0].val == "typedef" and any(h.val == "enum" for h in head):
                body = toks[i + 1:j]
                # An initialiser other than one integer literal (2 + 2, 1 << 1,
                # another enumerator) makes this enumerator and every implicit
                # one after it unknown (None); using one is a failure.
                vals, cur = {}, -1
                for a in split_args(body):
                    if not a:
                        continue
                    if len(a) == 1:
                        cur = None if cur is None else cur + 1
                    elif len(a) == 3 and a[1].val == "=" and a[2].kind == "num":
                        try:
                            cur = int(a[2].val.rstrip("uUlL"), 0)
                        except ValueError:
                            cur = None
                    else:
                        cur = None
                    vals[a[0].val] = cur
                if j + 1 < n and toks[j + 1].kind == "id":
                    enums[toks[j + 1].val] = vals
                enums.setdefault("", {}).update(vals)
                stmt_start = j + 1
                i = j + 1
                continue
            # name ( params ) {
            if len(head) >= 3 and head[-1].val == ")" and head[0].val != "typedef":
                k = len(head) - 1
                depth = 0
                while k >= 0:
                    if head[k].val == ")":
                        depth += 1
                    elif head[k].val == "(":
                        depth -= 1
                        if depth == 0:
                            break
                    k -= 1
                if k >= 1 and head[k - 1].kind == "id" and head[k - 1].val not in KEYWORDS \
                        and "=" not in [h.val for h in head[:k]]:
                    params = []
                    for a in split_args(head[k + 1:-1]):
                        ids = [x for x in a if x.kind == "id"]
                        if not ids or (len(a) == 1 and a[0].val == "void"):
                            continue
                        params.append((ids[-1].val, a[:-1]))
                    src.def_ids.add(id(head[k - 1]))
                    funcs.append(Func(src, head[k - 1].val,
                                      any(h.val == "static" for h in head[:k - 1]),
                                      params, i, j, head[:k - 1]))
            stmt_start = j + 1
            i = j + 1
            continue
        i += 1
    return funcs


def calls_in(func, names):
    """[(callee, [arg token lists], first token)] for calls to `names` in func."""
    toks, out = func.src.toks, []
    a, b = func.body
    i = a
    while i <= b:
        t = toks[i]
        if t.kind == "id" and t.val in names and i + 1 <= b and toks[i + 1].val == "(" \
                and not (i > a and toks[i - 1].val in (".", "->")):
            j = matching(toks, i + 1, "(", ")")
            out.append((t.val, split_args(toks[i + 2:j]), t))
            i = j + 1
            continue
        i += 1
    return out


# --------------------------------------------------------------------------
# Evaluation
# --------------------------------------------------------------------------

class Null:
    def __repr__(self):
        return "NULL"


NULL = Null()


class Sym(str):
    """A shaders_gen.h stage symbol, e.g. salias_frag."""


class Frame:
    """The context an expression is evaluated in: the function (here, the
    table initializer) whose file's macros it sees.  The other fields are
    the call-chain walk's, unused since the table (#340 P1b) replaced it."""
    def __init__(self, func, bind, parent, site):
        self.func, self.bind, self.parent, self.site = func, bind, parent, site


class Analysis:
    def __init__(self, root):
        self.root = root
        self.sources = {}
        self.enums = {}
        self.funcs = []
        self.shader_syms = {p.stem for p in (root / "engine/shaders").glob("*.glsl")}
        for d in SCAN_DIRS:
            for p in sorted(list((root / d).rglob("*.c")) + list((root / d).rglob("*.h"))):
                rel = p.relative_to(root).as_posix()
                if rel in EXCLUDED:
                    continue
                self.sources[rel] = Source(p, rel)
        if len(self.sources) < 100:
            raise Fail("scanned only %d C files under %s; is the tree there?"
                       % (len(self.sources), ", ".join(SCAN_DIRS)))
        # Header macros are visible everywhere, a .c file's own only in it.
        self.global_macros = {}
        for rel, s in self.sources.items():
            if rel.endswith(".h"):
                for k, v in s.macros.items():
                    self.global_macros.setdefault(k, []).extend(v)
        # engine/shaders/glonly/*.glsl reach the C as string macros (TIER_CORE).
        for p in sorted((root / "engine/shaders/glonly").glob("*.glsl")):
            body = p.read_text().replace("\r", "")
            self.global_macros[p.stem.upper()] = [
                Macro(p.stem.upper(), None, [Tok("str", body, "engine/shaders/glonly/" + p.name, 1)],
                      "engine/shaders/glonly/" + p.name, 1)]
        # A file whose unknown #if arms are both kept can come out with
        # unbalanced braces (host_cmd.c does).  Its functions are skipped;
        # that is safe because a "#define" literal, a stage symbol or a
        # glShaderSource call in it would then go unconsumed, which fails.
        self.unparsed = []
        for s in self.sources.values():
            try:
                self.funcs += parse_functions(s, self.enums)
            except Fail:
                self.unparsed.append(s.rel)
        self.by_name = {}
        for f in self.funcs:
            self.by_name.setdefault(f.name, []).append(f)
        self.consumed = set()      # id() of the "#define" literal tokens the table read
        self.reached = set()       # stage symbols that are a table row's body
        self.bodies = set()        # id() of the tokens that name them there

    # -- macros --------------------------------------------------------------
    def macro(self, name, file):
        defs = self.sources[file].macros.get(name) if file in self.sources else None
        if not defs:
            defs = self.global_macros.get(name)
        if not defs:
            return None
        if len({d.key() for d in defs}) > 1:
            raise Unresolved("macro %s has %d different definitions (%s)" % (
                name, len(defs), ", ".join("%s:%d" % (d.file, d.line) for d in defs)))
        return defs[0]

    def expand(self, toks, file, hide=frozenset(), local=frozenset()):
        out, i = [], 0
        while i < len(toks):
            t = toks[i]
            m = None
            if t.kind == "id" and t.val not in hide and t.val not in local:
                m = self.macro(t.val, file)
            if m is None:
                out.append(t)
                i += 1
                continue
            if m.params is None:
                out += self.expand(m.body, file, hide | {m.name}, local)
                i += 1
                continue
            if i + 1 >= len(toks) or toks[i + 1].val != "(":
                out.append(t)
                i += 1
                continue
            j = matching(toks, i + 1, "(", ")")
            args = split_args(toks[i + 2:j])
            if len(args) != len(m.params) and not (len(m.params) == 0 and args == []):
                raise Unresolved("%s:%d: macro %s takes %d arguments" % (t.file, t.line, m.name, len(m.params)))
            body, k = [], 0
            while k < len(m.body):
                b = m.body[k]
                if b.val == "#" and k + 1 < len(m.body) and m.body[k + 1].val in m.params:
                    arg = args[m.params.index(m.body[k + 1].val)]
                    body.append(Tok("str", " ".join(a.val for a in arg), t.file, t.line))
                    k += 2
                    continue
                if b.kind == "id" and b.val in m.params:
                    body += self.expand(args[m.params.index(b.val)], file, hide, local)
                else:
                    body.append(b)
                k += 1
            out += self.expand(body, file, hide | {m.name}, local)
            i = j + 1
        return out

    # -- expressions ----------------------------------------------------------
    def ev(self, toks, frame, asg):
        """Yield (value, asg) for every value toks can have.  A table row is
        compile-time text, so in practice one value: literals and macros
        concatenated, GLSL_STR stringized, integer + - * folded."""
        func = frame.func
        toks = self.expand(toks, func.src.rel, local=frozenset(p[0] for p in func.params))
        toks = strip_parens(toks)
        if not toks:
            raise Unresolved("empty expression")
        # Ternary: cond ? a : b
        q = top_index(toks, "?")
        if q is not None:
            c = top_index(toks, ":", q + 1)
            if c is None:
                raise Unresolved("%s:%d: malformed ?:" % (toks[0].file, toks[0].line))
            for cv, a2 in self.ev(toks[:q], frame, asg):
                truthy = not (cv is NULL or cv == 0 or cv == "0")
                yield from self.ev(toks[q + 1:c] if truthy else toks[c + 1:], frame, a2)
            return
        # Arithmetic on ints, C precedence: split at the last top-level + or -
        # (left-associative), else at the last top-level *.  A + or - right
        # after another operator is unary, which this does not evaluate.
        ops_add, ops_mul, depth = [], [], 0
        for k, tk in enumerate(toks):
            if tk.kind == "p" and tk.val in ("(", "[", "{"):
                depth += 1
            elif tk.kind == "p" and tk.val in (")", "]", "}"):
                depth -= 1
            elif depth == 0 and tk.kind == "p" and tk.val in ("+", "-", "*", "/", "%", "<<", ">>",
                                                             "&", "|", "^"):
                if k == 0 or (toks[k - 1].kind == "p" and toks[k - 1].val not in (")", "]")):
                    raise Unresolved("%s:%d: unary %s is not evaluated" % (tk.file, tk.line, tk.val))
                if tk.val in ("+", "-"):
                    ops_add.append(k)
                elif tk.val == "*":
                    ops_mul.append(k)
                else:
                    raise Unresolved("%s:%d: operator %s is not evaluated" % (tk.file, tk.line, tk.val))
        ops = ops_add or ops_mul
        if ops:
            k = ops[-1]
            for lv, a2 in self.ev(toks[:k], frame, asg):
                for rv, a3 in self.ev(toks[k + 1:], frame, a2):
                    if not (isinstance(lv, int) and isinstance(rv, int)):
                        raise Unresolved("%s:%d: arithmetic on non-integers" % (toks[k].file, toks[k].line))
                    yield {"+": lv + rv, "-": lv - rv, "*": lv * rv}[toks[k].val], a3
            return
        if len(toks) == 1:
            yield from self.primary(toks[0], frame, asg)
            return
        # Adjacent strings / string-valued names: concatenation.
        def concat(k, acc, a):
            if k == len(toks):
                yield acc, a
                return
            for v, a2 in self.primary(toks[k], frame, a):
                if not isinstance(v, str) or isinstance(v, Sym):
                    raise Unresolved("%s:%d: cannot concatenate %r" % (toks[k].file, toks[k].line, v))
                yield from concat(k + 1, acc + v, a2)
        yield from concat(0, "", asg)

    def primary(self, t, frame, asg):
        if t.kind == "str":
            if DEFINE_RE.search(t.val):
                self.consumed.add(id(t))
            yield t.val, asg
            return
        if t.kind == "num":
            yield int(t.val.rstrip("uUlL"), 0), asg
            return
        if t.kind != "id":
            raise Unresolved("%s:%d: cannot evaluate %r" % (t.file, t.line, t.val))
        name = t.val
        if name == "NULL":
            yield NULL, asg
            return
        if name in self.shader_syms:
            yield Sym(name), asg
            return
        if name in self.enums.get("", {}) and self.enums[""][name] is not None:
            yield self.enums[""][name], asg
            return
        raise Unresolved("%s:%d: %s is not a literal or a macro -- a program row is compile-time "
                         "text, and a run-time value has no finite set of variants to compile"
                         % (t.file, t.line, name))

    # -- the table ---------------------------------------------------------------
    def table(self, errors):
        """The rprog_defs rows, desktop view: [Row].  A Row's stages are
        (header text, body Sym, blob, where), its `where` file:line."""
        src = self.sources.get(TABLE_FILE)
        if src is None:
            errors.append("%s is missing; the program table %s lives there" % (TABLE_FILE, TABLE_NAME))
            return []
        toks, start = src.toks, None
        for i, t in enumerate(toks):
            if t.kind == "id" and t.val == TABLE_NAME and i + 1 < len(toks) and toks[i + 1].val == "[":
                j = matching(toks, i + 1, "[", "]")
                if j + 2 < len(toks) and toks[j + 1].val == "=" and toks[j + 2].val == "{":
                    if start is not None:
                        errors.append("%s:%d: a second %s initializer" % (t.file, t.line, TABLE_NAME))
                    start = j + 2
        if start is None:
            errors.append("%s: no `%s[...] = { ... }` initializer" % (TABLE_FILE, TABLE_NAME))
            return []
        end = matching(toks, start, "{", "}")
        frame = Frame(Func(src, TABLE_NAME, True, [], start, end), {}, None, None)
        rows = []
        for entry in split_args(toks[start + 1:end]):
            if not entry:
                continue
            where = "%s:%d" % (entry[0].file, entry[0].line)
            if not (len(entry) >= 6 and entry[0].val == "[" and entry[1].kind == "id" and
                    entry[2].val == "]" and entry[3].val == "=" and entry[4].val == "{" and
                    matching(entry, 4, "{", "}") == len(entry) - 1):
                errors.append("%s: a %s row must be `[RPROG_X] = { name, link, { stages } }`"
                              % (where, TABLE_NAME))
                continue
            row = Row(entry[1].val, where)
            fields = split_args(entry[5:-1])
            if len(fields) != 3 or len(fields[0]) != 1 or fields[0][0].kind != "str" or \
                    len(fields[1]) != 1 or fields[1][0].kind != "id" or \
                    not fields[2] or fields[2][0].val != "{" or \
                    matching(fields[2], 0, "{", "}") != len(fields[2]) - 1:
                errors.append("%s: %s: want `{ \"name\", RPROG_LINK_*, { stages } }`" % (where, row.id))
                continue
            row.name, row.link = fields[0][0].val, fields[1][0].val
            for st in split_args(fields[2][1:-1]):
                if not st:
                    continue
                sw = "%s:%d" % (st[0].file, st[0].line)
                parts = split_args(st[1:-1]) if st[0].val == "{" and st[-1].val == "}" else []
                if len(parts) != 3 or len(parts[1]) != 1 or len(parts[2]) != 1 or \
                        parts[2][0].kind != "str":
                    errors.append("%s: %s: a stage is `{ header, body, \"blob\" }`" % (sw, row.id))
                    continue
                try:
                    hv = list(self.ev(parts[0], frame, {}))
                    bv = list(self.ev(parts[1], frame, {}))
                except Unresolved as e:
                    errors.append("%s: %s: %s" % (sw, row.id, e))
                    continue
                if len(hv) != 1 or not isinstance(hv[0][0], str) or isinstance(hv[0][0], Sym):
                    errors.append("%s: %s: the header chunk is not one string" % (sw, row.id))
                    continue
                if len(bv) != 1 or not isinstance(bv[0][0], Sym):
                    errors.append("%s: %s: the body is not an engine/shaders stage symbol"
                                  % (sw, row.id))
                    continue
                self.reached.add(str(bv[0][0]))
                self.bodies.update(id(x) for x in parts[1])
                row.stages.append((hv[0][0], bv[0][0], parts[2][0].val, sw))
            rows.append(row)
        return rows

    def programs(self, errors):
        """[(stage symbol, header text, where)] for every stage of every row."""
        out = []
        for row in self.table(errors):
            for k, (header, body, blob, where) in enumerate(row.stages):
                out.append((body, header, "%s %s[%s] stage %d" % (where, TABLE_NAME, row.id, k)))
        return out


class Row:
    def __init__(self, rid, where):
        self.id, self.where = rid, where
        self.name = self.link = None
        self.stages = []


# The loaders and the declarations: the only files where glShaderSource may be
# named other than as a call inside LEAF.  rg_null.c is SDL_GPU mode's
# generated null table (#340): it assigns and checks the pointer and never
# compiles a shader through it.
SOURCE_DECL_FILES = {"engine/h2shared/gl_vidsdl.c", "engine/h2shared/gl_func.h",
                     "engine/h2shared/rg_null.c"}


def hidden_paths(an, errors):
    """A shader compile the table does not describe: glShaderSource named
    anywhere but a call inside LEAF (or the loader and declaration files),
    LEAF defined more than once or called from anywhere but CREATOR, or
    either named other than as its definition, prototype or such a call --
    through a macro or a function pointer, which this could not follow."""
    leaves = [f for f in an.funcs if f.name == LEAF]
    creators = [f for f in an.funcs if f.name == CREATOR]
    if len(leaves) != 1 or leaves[0].src.rel != TABLE_FILE:
        errors.append("want exactly one %s, in %s; found %s" % (
            LEAF, TABLE_FILE, ", ".join(f.where() for f in leaves) or "none"))
    if len(creators) != 1 or creators[0].src.rel != TABLE_FILE:
        errors.append("want exactly one %s, in %s; found %s" % (
            CREATOR, TABLE_FILE, ", ".join(f.where() for f in creators) or "none"))
    gss = {"glShaderSource", "glShaderSource_fp"}
    for s in an.sources.values():
        depth = 0
        for k, tk in enumerate(s.toks):
            if tk.kind == "p" and tk.val == "{":
                depth += 1
            elif tk.kind == "p" and tk.val == "}":
                depth -= 1
            if tk.kind != "id" or tk.val not in gss | {LEAF}:
                continue
            nxt = s.toks[k + 1].val if k + 1 < len(s.toks) else None
            if tk.val in gss:
                in_leaf = any(f.src is s and f.body[0] <= k <= f.body[1] for f in leaves)
                if (in_leaf and nxt == "(") or (s.rel in SOURCE_DECL_FILES and nxt != "("):
                    continue
                errors.append("%s:%d: %s named outside %s; a shader compiled there is no "
                              "program row, so test_shader_variants.py cannot see it"
                              % (tk.file, tk.line, tk.val, LEAF))
                continue
            if id(tk) in s.def_ids:
                continue
            in_creator = any(f.src is s and f.body[0] <= k <= f.body[1] for f in creators)
            if in_creator and nxt == "(":
                continue
            if depth == 0 and nxt == "(":
                try:
                    j = matching(s.toks, k + 1, "(", ")")
                except Fail:
                    j = None
                if j is not None and j + 1 < len(s.toks) and s.toks[j + 1].val == ";":
                    continue        # prototype
            errors.append("%s:%d: %s is named here other than as %s's call; a stage compiled "
                          "through it would not be a program row" % (tk.file, tk.line, LEAF, CREATOR))
        for ms in s.macros.values():
            for m in ms:
                for b in m.body:
                    if b.kind == "id" and (b.val == LEAF or (b.val in gss and s.rel not in SOURCE_DECL_FILES)):
                        errors.append("%s:%d: macro %s names %s; a shader built through it would "
                                      "escape test_shader_variants.py" % (b.file, b.line, m.name, b.val))


def strip_parens(toks):
    while True:
        if len(toks) >= 2 and toks[0].val == "(" and matching(toks, 0, "(", ")") == len(toks) - 1:
            toks = toks[1:-1]
            continue
        # A cast: ( type [*...] ) expr
        if len(toks) >= 4 and toks[0].val == "(":
            j = matching(toks, 0, "(", ")")
            inner = toks[1:j]
            if inner and all(x.kind == "id" or x.val == "*" for x in inner) and \
                    j + 1 < len(toks) and inner[0].kind == "id":
                toks = toks[j + 1:]
                continue
        return toks


def top_index(toks, val, start=0):
    depth = 0
    for k in range(start, len(toks)):
        t = toks[k]
        if t.kind == "p" and t.val in "([{":
            depth += 1
        elif t.kind == "p" and t.val in ")]}":
            depth -= 1
        elif depth == 0 and t.kind == "p" and t.val == val:
            return k
    return None


# --------------------------------------------------------------------------
# Manifest and shader sources
# --------------------------------------------------------------------------

def read_manifest(path, errors):
    variants, consts = [], {}
    if not path.exists():
        errors.append("%s is missing" % path)
        return variants, consts
    for n, raw in enumerate(path.read_text().replace("\r", "").split("\n"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        w = line.split()
        where = "variants.txt:%d" % n
        if re.search(r"[\"'\\]", line):
            errors.append("%s: no quotes or backslashes (EmbedShaders would strip them)" % where)
            continue
        if w[0] == "const":
            if len(w) != 3 or not re.match(r"^[A-Z_][A-Z0-9_]*$", w[1]):
                errors.append("%s: want `const NAME value`" % where)
                continue
            if w[1] in consts:
                errors.append("%s: const %s listed twice" % (where, w[1]))
            consts[w[1]] = (w[2], where)
        elif w[0] == "variant" and len(w) >= 4:
            if not re.match(r"^[a-z0-9_]+\.(vert|frag|comp)$", w[1]) or not re.match(r"^[a-z0-9_]+$", w[2]):
                errors.append("%s: bad stage or name in %r" % (where, line))
                continue
            defs = {}
            for d in w[3:]:
                m = re.match(r"^([A-Z_][A-Z0-9_]*)=([^=]+)$", d)
                if not m:
                    errors.append("%s: bad define %r, want NAME=value" % (where, d))
                    continue
                if m.group(1) in defs:
                    errors.append("%s: %s set twice" % (where, m.group(1)))
                if RESERVED.match(m.group(1)):
                    errors.append("%s: %s is reserved by glslang" % (where, m.group(1)))
                defs[m.group(1)] = m.group(2)
            variants.append((w[1], w[2], defs, where))
        else:
            errors.append("%s: want `variant <stage> <name> NAME=value ...` or `const NAME value`" % where)
    return variants, consts


def expand_includes(shader_dir, text):
    """EmbedShaders.cmake's include expansion, pass for pass."""
    for _ in range(32):
        m = re.search(r'#include "([^"]+)"', text)
        if not m:
            break
        inc = (shader_dir / m.group(1)).read_text()
        text = text.replace('#include "%s"\n' % m.group(1), inc)
    return text


def stage_file(sym):
    m = re.match(r"^(.+)_(vert|frag|comp)$", sym)
    return "%s.%s" % (m.group(1), m.group(2)) if m else None


def parse_header(text):
    """{NAME: value} from a header chunk, or raise Unresolved."""
    defs = {}
    for line in text.split("\n"):
        s = line.strip()
        if not s or s.startswith("#version") or s.startswith("precision "):
            continue
        m = re.match(r"^#\s*define\s+([A-Za-z_]\w*)(?:\s+(.*?))?\s*$", s)
        if not m:
            raise Unresolved("header line %r is neither #version, precision nor #define; an "
                             "#extension or #pragma makes a different program the manifest cannot "
                             "express" % s)
        if m.group(1) in defs and defs[m.group(1)] != (m.group(2) or ""):
            raise Unresolved("header defines %s twice" % m.group(1))
        defs[m.group(1)] = m.group(2) or ""
    return defs


def check_invert_y(root, errors):
    """glslang's --invert-y is implemented by its HLSL front end only: on GLSL
    input (all of engine/shaders) it is accepted and does nothing -- every
    stage, vertex stages included, compiles to the same bytes with and
    without it (glslang 16.3.0, #340 P-1b).  Passing it would read as the Y
    orientation being handled when it is not, so it is refused here."""
    t = re.sub(r"#[^\n]*", "", (root / "engine/cmake/CompileSpirv.cmake").read_text())
    if re.search(r"(?<![\w-])--(?:invert-y|iy)(?![\w-])", t):
        errors.append("CompileSpirv.cmake passes --invert-y, which glslang ignores for GLSL input "
                      "(HLSL front end only); the SDL_GPU Y flip has to be done another way (#340)")


# --------------------------------------------------------------------------

def offline_copies(shader_dir, variants, consts, errors):
    """EmbedShaders.cmake's offline copies, as it names them: (bodies,
    offline, vfiles).  bodies maps a stage file ("salias.frag") to its
    include-expanded text; offline maps (stage, frozenset of defines) to the
    copy's name -- the plain stage, its OIT twin, each variants.txt line --
    and vfiles the variant copies to (stage, defines, where)."""
    bodies = {}
    for p in sorted(shader_dir.glob("*.glsl")):
        st = stage_file(p.stem)
        if st:
            bodies[st] = expand_includes(shader_dir, p.read_text())

    # Offline copies: plain, OIT twin, manifest variants.
    offline = {}
    for st, body in bodies.items():
        offline[(st, frozenset())] = st
        if "#ifdef OIT\n" in body:
            b, e = st.rsplit(".", 1)
            offline[(st, frozenset({("OIT", "1")}))] = "%s_oit.%s" % (b, e)
    vfiles = {}
    for st, name, defs, where in variants:
        if st not in bodies:
            errors.append("%s: no stage %s in engine/shaders" % (where, st))
            continue
        for d in defs:
            if d in consts:
                errors.append("%s: %s is a const; it cannot also be a variant axis" % (where, d))
        key = (st, frozenset(defs.items()))
        b, e = st.rsplit(".", 1)
        fname = "%s_%s.%s" % (b, name, e)
        if key in offline:
            errors.append("%s: %s duplicates %s" % (where, fname, offline[key]))
            continue
        if fname in offline.values() or fname in vfiles:
            errors.append("%s: %s is already another offline copy's name" % (where, fname))
            continue
        offline[key] = fname
        vfiles[fname] = (st, defs, where)
    return bodies, offline, vfiles


def stage_defines(header, st, consts):
    """The variant axes a header chunk sets on stage `st`: its #defines less
    the manifest's consts.  Raises Unresolved on a line the manifest cannot
    express."""
    return {k: v for k, v in parse_header(header).items() if k not in consts}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument("--vk-dir", type=Path, help="EmbedShaders VK_DIR to check against the manifest")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)
    root = args.root.resolve()
    shader_dir = root / "engine/shaders"
    errors = []

    variants, consts = read_manifest(shader_dir / "variants.txt", errors)
    check_invert_y(root, errors)
    bodies, offline, vfiles = offline_copies(shader_dir, variants, consts, errors)

    # The C.
    try:
        an = Analysis(root)
        progs = an.programs(errors)
    except (Fail, Unresolved) as e:
        errors.append(str(e))
        progs, an = [], None

    used_variants, used_consts, runtime = set(), set(), {}
    for src, header, chain in progs:
        if not isinstance(src, Sym):
            errors.append("%s: compiles an inline source %r, not an engine/shaders stage; SDL_GPU "
                          "only gets blobs for engine/shaders files" % (chain, str(src)[:40]))
            continue
        an.reached.add(str(src))
        st = stage_file(src)
        try:
            defs = parse_header(header)
        except Unresolved as e:
            errors.append("%s (%s): %s" % (chain, st, e))
            continue
        axes = {}
        for k, v in defs.items():
            if k in consts:
                used_consts.add(k)
                if v != consts[k][0]:
                    errors.append("%s (%s): the C passes %s=%s, %s pins %s" %
                                  (chain, st, k, v, consts[k][1], consts[k][0]))
                body = bodies.get(st, "")
                if re.search(r"\b%s\b" % k, body):
                    dflt = re.findall(r"^\s*#\s*define\s+%s\s+(\S+)" % k, body, re.M)
                    if dflt != [consts[k][0]]:
                        errors.append("%s reads %s, which the C sets to %s, but its #ifndef default "
                                      "is %s; the plain offline compile would not be the GL program"
                                      % (st, k, v, dflt or "missing"))
            else:
                axes[k] = v
        key = (st, frozenset(axes.items()))
        runtime.setdefault(key, chain)
        if key in offline:
            if offline[key] in vfiles:
                used_variants.add(offline[key])
        else:
            shown = " ".join("%s=%s" % kv for kv in sorted(axes.items()))
            unknown = sorted(k for k in axes if not any(k in d for _, _, d, _ in variants) and k != "OIT")
            hint = (" (%s not enumerated in variants.txt at all)" % ", ".join(unknown)) if unknown else ""
            errors.append("%s builds %s with %s, and no offline copy has that define set%s; add "
                          "`variant %s <name> %s` to engine/shaders/variants.txt"
                          % (chain, st, shown, hint, st, shown))

    if an is not None:
        if len(progs) < 40:
            errors.append("the table holds only %d stage compiles; is the C there?" % len(progs))
        # Nothing escaped the table.
        for s in an.sources.values():
            for t in s.toks + [b for ms in s.macros.values() for m in ms for b in m.body]:
                if t.kind == "str" and DEFINE_RE.search(t.val) and id(t) not in an.consumed:
                    errors.append("%s:%d: %r is no program row's header; "
                                  "test_shader_variants.py cannot see what it is compiled into"
                                  % (t.file, t.line, t.val[:50]))
                # A stage symbol is named by its rows and nowhere else: the
                # engine has no other use for a shader's source than compiling
                # it, and any other compile would be no row.
                if t.kind == "id" and t.val in an.shader_syms and id(t) not in an.bodies:
                    errors.append("%s:%d: %s is named outside the program table (%s); a stage is "
                                  "compiled only as a row's body" % (t.file, t.line, t.val, TABLE_NAME))
        hidden_paths(an, errors)
        for (st, axes), fname in sorted(offline.items()):
            if axes == frozenset({("OIT", "1")}) and (st, axes) not in runtime:
                errors.append("%s: EmbedShaders writes %s, but no program the C builds is %s with "
                              "OIT=1" % (st, fname, st))
        for fname, (st, defs, where) in sorted(vfiles.items()):
            if fname not in used_variants:
                errors.append("%s: %s (%s) is built by no program the C can request; remove it"
                              % (where, fname, " ".join("%s=%s" % kv for kv in defs.items())))
        for k, (v, where) in sorted(consts.items()):
            if k not in used_consts:
                errors.append("%s: const %s is passed by no program the C builds; remove it" % (where, k))
        # Every shader-side default of a const agrees with it.
        for p in sorted(list(shader_dir.glob("*.glsl")) + list(shader_dir.glob("*.inc"))):
            for m in re.finditer(r"^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)\s+(\S+)", p.read_text(), re.M):
                if m.group(1) in consts and m.group(2) != consts[m.group(1)][0]:
                    errors.append("engine/shaders/%s defaults %s to %s, %s pins %s"
                                  % (p.name, m.group(1), m.group(2), consts[m.group(1)][1],
                                     consts[m.group(1)][0]))

    if args.vk_dir is not None:
        have = {p.name for p in args.vk_dir.iterdir() if p.suffix in (".vert", ".frag", ".comp")} \
            if args.vk_dir.is_dir() else set()
        want = set(offline.values())
        for n in sorted(want - have):
            errors.append("%s: no %s" % (args.vk_dir, n))
        for n in sorted(have - want):
            errors.append("%s: %s is not a stage, OIT twin or variants.txt variant" % (args.vk_dir, n))
        for fname, (st, defs, where) in sorted(vfiles.items()):
            p = args.vk_dir / fname
            if not p.exists():
                continue
            lines = p.read_text().split("\n")
            head = lines[1:1 + len(defs)]
            wanthead = ["#define %s %s" % kv for kv in defs.items()]
            if lines[0] != "#version 450 core" or head != wanthead or \
                    "\n".join(lines[1 + len(defs):]) != bodies[st]:
                errors.append("%s: not #version, then %s, then %s's expanded source"
                              % (p, " / ".join(wanthead), st))

    if args.verbose:
        if an is not None and an.unparsed:
            print("skipped (unbalanced once unknown #if arms are both kept): " + ", ".join(an.unparsed))
        for (st, axes), chain in sorted(runtime.items(), key=lambda kv: (kv[0][0], sorted(kv[0][1]))):
            print("%-22s %-26s %s" % (st, " ".join("%s=%s" % kv for kv in sorted(axes)) or "-",
                                      offline.get((st, axes), "MISSING")))
    if errors:
        for e in errors:
            print("FAIL: " + e, file=sys.stderr)
        print("%d shader-variant problem(s)" % len(errors), file=sys.stderr)
        return 1
    print("PASS shader variants: %d runtime stage builds, %d distinct, %d variants + %d consts in "
          "variants.txt%s (#340)"
          % (len(progs), len(runtime), len(vfiles), len(consts),
             ", %s matches" % args.vk_dir.name if args.vk_dir else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
