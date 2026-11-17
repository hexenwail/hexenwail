#!/usr/bin/env python3
"""Every vertex stage flips Y for SDL_GPU, and only for SDL_GPU (GitHub #340).

SDL_GPU's clip space is +Y up with framebuffer row 0 at the top -- its Vulkan
backend sets a negative-height viewport (SDL_gpu_vulkan.c,
VULKAN_INTERNAL_SetCurrentViewport: `y = viewport->y + viewport->h;
height = -viewport->h`) to match D3D12 and Metal.  GL puts NDC y = -1 on row 0.  So the
Vulkan SPIR-V negates gl_Position.y, through layout.inc's VK_FLIP_Y(), and
every render target keeps GL's row order in memory.  glslang's --invert-y
would have been the one-flag answer, but it is HLSL-only and a silent no-op on
GLSL (tools/test_shader_variants.py refuses it).

A stage that forgets the macro still compiles everywhere and draws upside
down only under SDL_GPU, so the rule is checked here rather than trusted:

Source mode (default, lint.yml):
  * layout.inc defines VK_FLIP_Y() as `gl_Position.y = -gl_Position.y;` in
    the VULKAN arm and as nothing in the GL_SPIRV and plain-GL arms (each arm
    evaluated as glslang's preprocessor would pick it);
  * every engine/shaders/*_vert.glsl reaches layout.inc through its #includes,
    ends main() with a bare VK_FLIP_Y() -- no trailing `;`, so the GL arms see
    no token at all, not even an empty statement -- outside any #if, and
    precedes every `return` in main() with one;
  * VK_FLIP_Y appears nowhere else: not in a fragment or compute stage, an
    .inc, or a function other than a vertex stage's main().
  engine/shaders/glonly/ is GL-only text (embedded verbatim, never compiled
  offline, never handed to SDL_GPU) and is out of scope.

SPIR-V mode (--spirv DIR, shaders-spirv.yml, after shaders_spirv):
  * every *.vert.vk.spv, in every block of its entry point that ends in
    OpReturn, stores OpFNegate(load gl_Position.y) back to gl_Position.y,
    with no later store to gl_Position in that block;
  * every *.vert.gl.spv has no such store -- the GL_SPIRV arm compiled to
    nothing;
  * with --sources VK_DIR (EmbedShaders.cmake's staging dir), every .vert
    there has both blobs, so a gate that compiled nothing cannot pass.

Stock python3 only.
"""
from pathlib import Path
import argparse
import re
import struct
import sys

ROOT = Path(__file__).resolve().parent.parent
FLIP_BODY = "gl_Position.y = -gl_Position.y;"
ARMS = {"vulkan": {"VULKAN"}, "gl_spirv": {"GL_SPIRV"}, "gl": set()}

errors = []


def fail(msg):
    errors.append(msg)


# --- source ------------------------------------------------------------------

def strip_comments(text):
    """Comments to spaces, newlines kept, so line numbers survive."""
    out = []
    i, n = 0, len(text)
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


def eval_cond(expr, defined, where):
    """#if / #elif expression: only defined(X), !, &&, || and parentheses."""
    py = expr
    py = re.sub(r"\bdefined\s*\(\s*(\w+)\s*\)", lambda m: " %s " % (m.group(1) in defined), py)
    py = re.sub(r"\bdefined\s+(\w+)", lambda m: " %s " % (m.group(1) in defined), py)
    py = py.replace("&&", " and ").replace("||", " or ")
    py = re.sub(r"!(?!=)", " not ", py)
    if not re.fullmatch(r"[\s()]*((True|False|and|or|not)[\s()]*)*", py):
        fail("%s: cannot evaluate `#if %s` (only defined(), !, &&, || are understood)" % (where, expr))
        return False
    return bool(eval(py))


def flip_definition(path, defined):
    """The VK_FLIP_Y() body layout.inc leaves defined under `defined`."""
    stack = []  # (taking, any_taken, parent_taking)
    taking = True
    body = None
    for ln, line in enumerate(strip_comments(path.read_text()).splitlines(), 1):
        s = line.strip()
        where = "%s:%d" % (path.name, ln)
        m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif|define|undef)\b(.*)", s)
        if not m:
            continue
        d, rest = m.group(1), m.group(2).strip()
        if d in ("ifdef", "ifndef", "if"):
            if d == "ifdef":
                c = rest in defined
            elif d == "ifndef":
                c = rest not in defined
            else:
                c = eval_cond(rest, defined, where)
            stack.append((taking, c))
            taking = taking and c
        elif d == "elif":
            parent, any_taken = stack[-1]
            c = (not any_taken) and eval_cond(rest, defined, where)
            stack[-1] = (parent, any_taken or c)
            taking = parent and c
        elif d == "else":
            parent, any_taken = stack[-1]
            stack[-1] = (parent, True)
            taking = parent and not any_taken
        elif d == "endif":
            taking, _ = stack.pop()
        elif taking and d == "define":
            dm = re.match(r"VK_FLIP_Y\b(\(\s*\))?(.*)", rest)
            if dm:
                if dm.group(1) is None:
                    fail("%s: VK_FLIP_Y must be function-like, VK_FLIP_Y()" % where)
                body = " ".join(dm.group(2).split())
        elif taking and d == "undef" and rest == "VK_FLIP_Y":
            body = None
        if taking and d == "define" and re.match(r"(VULKAN|GL_SPIRV)\b", rest):
            fail("%s: defines %s, which glslang predefines" % (where, rest.split()[0]))
    if stack:
        fail("%s: unterminated #if" % path.name)
    return body


def includes(path, seen=None):
    """Every file path reaches through #include, path itself first."""
    seen = seen if seen is not None else []
    if path in seen:
        return seen
    seen.append(path)
    if not path.is_file():
        return seen
    for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', strip_comments(path.read_text()), re.M):
        includes(path.parent / m.group(1), seen)
    return seen


TOKEN = re.compile(r"\n[ \t]*#[^\n]*|[A-Za-z_]\w*|\d[\w.]*|\S")


def tokens(text):
    """(token, line) pairs; a directive line is one token, `#name rest`.
    text starts with an extra newline so a directive on line 1 matches."""
    out = []
    for m in TOKEN.finditer(text):
        t, start = m.group(0), m.start()
        if t.startswith("\n"):
            t, start = "#" + " ".join(t.strip()[1:].split()), start + 1
        out.append((t, text.count("\n", 0, start)))
    return out


def find_main(toks):
    """(open index, close index) of main()'s braces, or None."""
    for i in range(len(toks) - 4):
        if [t for t, _ in toks[i:i + 4]] == ["void", "main", "(", ")"] and toks[i + 4][0] == "{":
            depth = 0
            for j in range(i + 4, len(toks)):
                depth += toks[j][0] == "{"
                depth -= toks[j][0] == "}"
                if depth == 0:
                    return i + 4, j
    return None


def check_stage(path):
    rel = path.relative_to(ROOT)
    if not any(p.name == "layout.inc" for p in includes(path)):
        fail("%s: does not #include \"layout.inc\", so VK_FLIP_Y() is undefined" % rel)
    toks = tokens("\n" + strip_comments(path.read_text()))
    span = find_main(toks)
    if not span:
        fail("%s: no `void main()` found" % rel)
        return
    lo, hi = span
    body = toks[lo + 1:hi]
    names = [t for t, _ in body]

    def is_call(k):
        return names[k:k + 3] == ["VK_FLIP_Y", "(", ")"]

    if len(names) < 3 or names[-3:] != ["VK_FLIP_Y", "(", ")"]:
        last = names[-1] if names else "{"
        fail("%s:%d: main() does not end with VK_FLIP_Y() (last token `%s`)"
             % (rel, toks[hi][1], last))
    else:
        depth = 0
        for t in names[:-3]:
            if re.match(r"#(if|ifdef|ifndef)\b", t):
                depth += 1
            elif t.startswith("#endif"):
                depth -= 1
        if depth:
            fail("%s:%d: main()'s closing VK_FLIP_Y() is inside an #if, so some builds skip it"
                 % (rel, body[-3][1]))
    for k, t in enumerate(names):
        if t == "VK_FLIP_Y" and names[k + 3:k + 4] == [";"]:
            fail("%s:%d: VK_FLIP_Y(); -- drop the `;`: the macro carries its own, and the GL arms "
                 "must see no token" % (rel, body[k][1]))
        if t == "return" and not (k >= 3 and is_call(k - 3)):
            fail("%s:%d: `return` in main() without VK_FLIP_Y() immediately before it"
                 % (rel, body[k][1]))
        # Every call must END a return path: one more anywhere else (a second
        # call on the same path) cancels the first and the stage draws
        # unflipped while every other rule still holds.
        if is_call(k) and not (k + 3 == len(names) or names[k + 3:k + 4] == ["return"]):
            fail("%s:%d: VK_FLIP_Y() that does not end a return path -- a second flip on "
                 "one path cancels the first" % (rel, body[k][1]))
    # A hand-written write to gl_Position's y would flip (or unflip) behind
    # the macro's back, in every arm.
    for k in range(len(toks) - 3):
        if toks[k][0] == "gl_Position" and toks[k + 1][0] == "." and \
                re.fullmatch(r"[xyzwrgbastpq]+", toks[k + 2][0]) and \
                re.search(r"[ygt]", toks[k + 2][0]) and \
                toks[k + 3][0] in ("=", "+", "-", "*", "/") and \
                ((toks[k + 3][0] == "=") != (toks[k + 4][0] == "=")):
            fail("%s:%d: writes gl_Position.%s by hand; Y orientation belongs to VK_FLIP_Y()"
                 % (rel, toks[k][1], toks[k + 2][0]))
    # Anywhere outside main() in this file is wrong.
    for k, (t, ln) in enumerate(toks):
        if t == "VK_FLIP_Y" and not (lo < k < hi):
            fail("%s:%d: VK_FLIP_Y outside main()" % (rel, ln))
        if t.startswith("#") and "VK_FLIP_Y" in t:
            fail("%s:%d: VK_FLIP_Y in a directive" % (rel, ln))


def source_mode(root):
    global ROOT
    ROOT = root
    sdir = root / "engine/shaders"
    layout = sdir / "layout.inc"
    want = {"vulkan": FLIP_BODY, "gl_spirv": "", "gl": ""}
    for arm, defs in ARMS.items():
        got = flip_definition(layout, defs)
        if got is None:
            fail("layout.inc: VK_FLIP_Y() is not defined in the %s arm" % arm)
        elif got != want[arm]:
            fail("layout.inc: VK_FLIP_Y() in the %s arm expands to `%s`, want `%s`"
                 % (arm, got, want[arm]))
    stages = sorted(sdir.glob("*_vert.glsl"))
    if not stages:
        fail("no engine/shaders/*_vert.glsl found")
    for st in stages:
        check_stage(st)
    for f in sorted(sdir.rglob("*")):
        if not f.is_file() or f in stages or f == layout:
            continue
        if f.parent.name == "glonly" and f.suffix == ".glsl":
            if "VK_FLIP_Y" in strip_comments(f.read_text()):
                fail("%s: glonly text is GL-only; VK_FLIP_Y means nothing there" % f.relative_to(root))
            continue
        if f.suffix in (".glsl", ".inc") and "VK_FLIP_Y" in strip_comments(f.read_text()):
            fail("%s: VK_FLIP_Y belongs only in a vertex stage's main()" % f.relative_to(root))
    return len(stages)


# --- SPIR-V -----------------------------------------------------------------

OP_ENTRY_POINT, OP_FUNCTION, OP_FUNCTION_END = 15, 54, 56
OP_VARIABLE, OP_LOAD, OP_STORE, OP_ACCESS_CHAIN = 59, 61, 62, 65
OP_CONSTANT, OP_TYPE_POINTER, OP_TYPE_STRUCT = 43, 32, 30
OP_DECORATE, OP_MEMBER_DECORATE, OP_LABEL = 71, 72, 248
OP_FNEGATE, OP_RETURN, OP_RETURN_VALUE = 127, 253, 254
OP_FUNCTION_CALL = 57
OP_IN_BOUNDS_ACCESS_CHAIN = 66
DEC_BUILTIN, BUILTIN_POSITION = 11, 0
SC_OUTPUT = 3


def parse_spirv(data):
    if len(data) < 20 or len(data) % 4:
        raise ValueError("not a SPIR-V module")
    magic = struct.unpack_from("<I", data)[0]
    end = "<" if magic == 0x07230203 else ">"
    if struct.unpack_from(end + "I", data)[0] != 0x07230203:
        raise ValueError("bad SPIR-V magic")
    words = struct.unpack(end + "%dI" % (len(data) // 4), data)
    insts, i = [], 5
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0 or i + wc > len(words):
            raise ValueError("truncated instruction at word %d" % i)
        insts.append((op, words[i + 1:i + wc]))
        i += wc
    return insts


def flips(data):
    """([ok per OpReturn block of the vertex entry point], number of y-flip
    stores in the entry point).  A block is ok when it holds exactly one
    gl_Position.y = -gl_Position.y and that store is its last write to
    gl_Position, with no call after it."""
    insts = parse_spirv(data)
    entry = None
    consts, ptr_pointee, var_type = {}, {}, {}
    pos_member = {}   # struct id -> member index carrying BuiltIn Position
    pos_var = set()   # variables that ARE gl_Position
    for op, w in insts:
        if op == OP_ENTRY_POINT and w[0] == 0:
            entry = w[1]
        elif op == OP_CONSTANT and len(w) == 3:
            consts[w[1]] = w[2]
        elif op == OP_TYPE_POINTER:
            ptr_pointee[w[0]] = w[2]
        elif op == OP_VARIABLE and w[2] == SC_OUTPUT:
            var_type[w[1]] = w[0]
        elif op == OP_MEMBER_DECORATE and w[2] == DEC_BUILTIN and w[3] == BUILTIN_POSITION:
            pos_member[w[0]] = w[1]
        elif op == OP_DECORATE and w[1] == DEC_BUILTIN and w[2] == BUILTIN_POSITION:
            pos_var.add(w[0])
    if entry is None:
        raise ValueError("no Vertex OpEntryPoint")
    block_var = {v: pos_member[ptr_pointee[t]] for v, t in var_type.items()
                 if ptr_pointee.get(t) in pos_member}

    chains = {}  # access-chain result -> (base pointer, (const idx, ...))

    def path(ptr):
        """(var, indices) for a pointer into gl_Position, None otherwise; the
        indices are counted from gl_Position itself, None for the whole
        gl_PerVertex block.  Chains of chains are followed."""
        if ptr in pos_var:
            return (ptr, ())
        if ptr in block_var:
            return (ptr, "block")
        if ptr in chains:
            base, idx = chains[ptr]
            bp = path(base)
            if bp is None:
                return None
            var, bidx = bp
            if bidx == "block":
                if not idx:
                    return (var, "block")
                if idx[0] != block_var[var]:
                    return None
                return (var, idx[1:])
            return (var, bidx + idx)
        return None

    def is_flip(store_i, val, block):
        """Store `val` to gl_Position.y is the negation of a load of
        gl_Position.y made in this block after its last other gl_Position
        store -- so it negates the value the shader computed, not a stale
        copy."""
        if val not in negs or negs[val] not in loads:
            return False
        src_ptr, load_i = loads[negs[val]]
        sp = path(src_ptr)
        if sp is None or sp[1] != (1,):
            return False
        earlier = [i for i, kind in block if kind == "store" and i < store_i]
        return load_i in block_loads and load_i > max(earlier, default=-1)

    results, loads, negs = [], {}, {}
    block, block_loads, negstores = [], set(), 0
    in_entry = False
    for i, (op, w) in enumerate(insts):
        if op == OP_FUNCTION:
            in_entry = w[1] == entry
            continue
        if not in_entry:
            continue
        if op == OP_FUNCTION_END:
            in_entry = False
        elif op == OP_LABEL:
            block, block_loads = [], set()
        elif op in (OP_ACCESS_CHAIN, OP_IN_BOUNDS_ACCESS_CHAIN):
            chains[w[1]] = (w[2], tuple(consts.get(x, -1) for x in w[3:]))
        elif op == OP_LOAD:
            loads[w[1]] = (w[2], i)
            block_loads.add(i)
        elif op == OP_FNEGATE:
            negs[w[1]] = w[2]
        elif op == OP_STORE:
            pp = path(w[0])
            if pp is not None:
                flip = pp[1] == (1,) and is_flip(i, w[1], block)
                negstores += flip
                block.append((i, "flip" if flip else "store"))
        elif op == OP_FUNCTION_CALL:
            # a callee may write gl_Position; it must not run after the flip
            block.append((i, "call"))
        elif op in (OP_RETURN, OP_RETURN_VALUE):
            kinds = [k for _, k in block]
            results.append(kinds.count("flip") == 1 and kinds[-1:] == ["flip"])
    return results, negstores


def spirv_mode(spv_dir, sources):
    vk = sorted(spv_dir.glob("*.vert.vk.spv"))
    gl = sorted(spv_dir.glob("*.vert.gl.spv"))
    if not vk:
        fail("%s: no *.vert.vk.spv" % spv_dir)
    if sources:
        for v in sorted(sources.glob("*.vert")):
            for suffix in ("vk.spv", "gl.spv"):
                if not (spv_dir / ("%s.%s" % (v.name, suffix))).is_file():
                    fail("%s: no %s.%s -- the stage was not compiled" % (v.name, v.name, suffix))
    for f in vk:
        try:
            r, nflip = flips(f.read_bytes())
        except ValueError as e:
            fail("%s: %s" % (f.name, e))
            continue
        if not r:
            fail("%s: entry point has no OpReturn" % f.name)
        elif not all(r):
            fail("%s: %d of %d return paths leave gl_Position.y unflipped"
                 % (f.name, r.count(False), len(r)))
        elif nflip != len(r):
            fail("%s: %d y-flip stores for %d return paths -- a second flip cancels the first"
                 % (f.name, nflip, len(r)))
    for f in gl:
        try:
            r, nflip = flips(f.read_bytes())
        except ValueError as e:
            fail("%s: %s" % (f.name, e))
            continue
        if nflip:
            fail("%s: GL SPIR-V negates gl_Position.y -- VK_FLIP_Y() is not empty outside VULKAN"
                 % f.name)
    return len(vk), len(gl)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--spirv", type=Path, help="CompileSpirv.cmake output dir")
    ap.add_argument("--sources", type=Path, help="EmbedShaders.cmake VK_DIR (with --spirv)")
    a = ap.parse_args(argv)
    if a.spirv:
        nvk, ngl = spirv_mode(a.spirv, a.sources)
        what = "%d Vulkan and %d GL vertex SPIR-V modules" % (nvk, ngl)
    else:
        what = "%d vertex stages" % source_mode(a.root.resolve())
    for e in errors:
        print("FAIL: " + e)
    if errors:
        print("test_vk_flip_y: %d failure(s) over %s" % (len(errors), what))
        return 1
    print("test_vk_flip_y: OK, %s" % what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
