#!/usr/bin/env python3
"""No GLSL #version string is left in the engine's C (#450).

Every #version / precision preamble the GL side hands to glShaderSource lives
in engine/shaders/glonly/ (tier_core.glsl, tier_es.glsl, tier_es_pp.glsl), and
EmbedShaders.cmake turns each into a string-literal macro in shaders_gen.h.
The WebGL2 canvas blit of the software renderer lives there too.  So no C
string literal under engine/h2shared or engine/hexen2 may contain "#version":
one that does is a shader, or a tier header, that went back into C.

What is scanned, per .c/.h, after joining backslash-newlines and blanking
comments:
  - every string literal, plain or prefixed (L, u, U, u8) or raw (R"x(...)x"),
    with its escapes decoded, so "\\x23version" counts;
  - every run of adjacent literals joined as the compiler joins them, so
    "#ver" "sion" counts;
  - the code between literals, for a "#version" that a stringizing macro
    (#define S(x) #x ... S(#version 430 core)) would turn into a literal.
A comment that mentions #version is fine; it never reaches a driver.
"""
from bisect import bisect_right
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DIRS = [ROOT / "engine/h2shared", ROOT / "engine/hexen2"]
NEEDLE = "#version"

ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f",
           "v": "\v", "\\": "\\", "'": "'", '"': '"', "?": "?"}


def decode(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\" or i + 1 == len(body):
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n == "x":
            m = re.match(r"[0-9A-Fa-f]+", body[i + 2:])
            out.append(chr(int(m.group(0), 16) & 0x10FFFF) if m else "x")
            i += 2 + (len(m.group(0)) if m else 0)
        elif n in "01234567":
            m = re.match(r"[0-7]{1,3}", body[i + 1:])
            out.append(chr(int(m.group(0), 8)))
            i += 1 + len(m.group(0))
        else:
            out.append(ESCAPES.get(n, n))
            i += 2
    return "".join(out)


RAW = re.compile(r'(?:u8|[LuU])?R"([^ ()\\\t\n]{0,16})\(')
STR = re.compile(r'(?:u8|[LuU])?"')
OPEN = re.compile(r'/[*/]|(?:u8|[LuU])?R?"|\'')


def blank(s):
    """s as spaces, newlines kept: the blanked code keeps the joined text's
    offsets, which is what `joins` is in."""
    return re.sub(r"[^\n]", " ", s)


def lex(text):
    """(literals, code): literals is [(line, value)] in order, each tagged with
    whether only whitespace separates it from the previous one; code is the
    text with comments and literals blanked to spaces, same length."""
    # Phase 2 joins backslash-newlines; `joins` keeps the reported line numbers
    # those of the file, not of the joined text.
    text = text.replace("\\\r\n", "\\\n")
    joins, parts, off = [], text.split("\\\n"), 0
    for part in parts[:-1]:
        off += len(part)
        joins.append(off)
    text = "".join(parts)
    lits, code = [], []
    i, line, n = 0, 1, len(text)
    gap = None  # code text since the last literal; None before the first
    while i < n:
        m = OPEN.search(text, i)
        j = n if m is None else m.start()
        if j > i:
            chunk = text[i:j]
            code.append(chunk)
            if gap is not None:
                gap += chunk
            line += chunk.count("\n")
            i = j
            continue
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            chunk = text[i:j]
            line += chunk.count("\n")
            code.append(blank(chunk))
            if gap is not None:
                gap += " "
            i = j
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            code.append(blank(text[i:j]))
            i = j
            continue
        m = RAW.match(text, i)
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            end = ")" + m.group(1) + '"'
            j = text.find(end, m.end())
            j = n if j < 0 else j
            value = text[m.end():j]
            lits.append((line + bisect_right(joins, i), value,
                         gap is not None and gap.strip() == ""))
            line += value.count("\n")
            code.append(blank(text[i:j + len(end)]))
            gap = ""
            i = j + len(end)
            continue
        m = STR.match(text, i)
        if m and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            j = m.end()
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            value = decode(text[m.end():j])
            lits.append((line + bisect_right(joins, i), value,
                         gap is not None and gap.strip() == ""))
            code.append(blank(text[i:j + 1]))
            gap = ""
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            code.append(blank(text[i:j + 1]))
            if gap is not None:
                gap += "x"
            i = j + 1
            continue
        code.append(c)
        if gap is not None:
            gap += c
        if c == "\n":
            line += 1
        i += 1
    return lits, "".join(code), joins


def scan(path):
    """[(line, what)] for every #version this file would hand to C as data."""
    lits, code, joins = lex(path.read_text(errors="replace"))
    hits = []
    run_line, run = None, ""
    for line, value, adjacent in lits + [(None, "", False)]:
        if adjacent:
            run += value
            continue
        if run_line is not None and NEEDLE in run:
            hits.append((run_line, f"string literal {run[:60]!r}"))
        run_line, run = line, value
    for m in re.finditer(r"#\s*version\b", code):
        hits.append((code.count("\n", 0, m.start()) + 1 + bisect_right(joins, m.start()),
                     "#version in code, where a stringizing macro makes it a literal"))
    return sorted(set(hits)), len(lits)


def main():
    errors, files, literals = [], 0, 0
    for d in DIRS:
        for path in sorted(list(d.rglob("*.c")) + list(d.rglob("*.h"))):
            files += 1
            hits, count = scan(path)
            literals += count
            for line, what in hits:
                errors.append(f"{path.relative_to(ROOT)}:{line}: {what}; the text belongs "
                              "in engine/shaders/glonly (see its README)")
    # A scan that read nothing passes vacuously; the two dirs hold hundreds of
    # files and tens of thousands of literals.
    if files < 100 or literals < 5000:
        errors.append(f"scanned only {files} files / {literals} literals; is the tree there?")
    if errors:
        sys.exit("FAIL:\n  " + "\n  ".join(errors))
    print(f"PASS no #version string in the C ({files} files, {literals} literals) (#450)")


if __name__ == "__main__":
    main()
