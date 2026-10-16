#!/usr/bin/env python3
"""Mods menu labels must stay distinct when they are shortened (GitHub #136).

M_Mods_FitLabel used to cut long titles at the end, so "Mission Pack by Rino"
and "Mission Pack by Rino 2" (mpbyrino, mpbyrino2) both drew as the same row.
It now puts the ellipsis in the middle and keeps the tail.

The real function is extracted from engine/hexen2/menu.c, compiled with the
host C compiler ($CC, default cc) and run against:
  - the two titles from the issue, which must differ;
  - the corpus's longest dirname, thebarongastonehousebyrino;
  - labels at, one over and well over the column, which must fit exactly;
  - short labels, which must come back untouched.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MENU = ROOT / "engine/hexen2/menu.c"


def function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def define(text, name, default=None):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, text)
    if m:
        return int(m.group(1))
    if default is None:
        sys.exit("FAIL: %s not defined in %s" % (name, MENU))
    return default


CASES = [
    "Mission Pack by Rino",
    "Mission Pack by Rino 2",
    "thebarongastonehousebyrino",
    "Hexen II",
    "",
    "x" * 19,
    "y" * 20,
    "z" * 80,
]


def main():
    # An optional path checks another copy of menu.c, e.g. master's, to show
    # the test fails without the fix.
    text = (Path(sys.argv[1]) if len(sys.argv) > 1 else MENU).read_text()
    width = define(text, "MODS_LABEL_MAX")
    fit = function(text, "static const char *M_Mods_FitLabel (const char *label)")

    program = "\n".join([
        "#include <stdio.h>",
        "#include <string.h>",
        # The engine's strlcpy/strlcat, for a version that calls them.
        "size_t q_strlcpy (char *d, const char *s, size_t n)",
        "{ size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1;"
        " memcpy(d, s, c); d[c] = 0; } return l; }",
        "size_t q_strlcat (char *d, const char *s, size_t n)",
        "{ size_t l = strlen(d); return l + q_strlcpy(d + l, s, n > l ? n - l : 0); }",
        "#define MODS_LABEL_MAX %d" % width,
        # Unused by the pre-#136 function, so absent there.
        "#define MODS_LABEL_TAIL %d" % define(text, "MODS_LABEL_TAIL", 0),
        fit,
        "int main (int argc, char **argv)",
        "{",
        "\tint i;",
        "\tfor (i = 1; i < argc; i++)",
        "\t\tprintf (\"%s\\n\", M_Mods_FitLabel (argv[i]));",
        "\treturn 0;",
        "}",
    ])

    with tempfile.TemporaryDirectory() as work:
        src = Path(work) / "fit.c"
        exe = Path(work) / "fit"
        src.write_text(program)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall",
                        "-Wextra", "-Werror", "-o", str(exe), str(src)],
                       check=True)
        out = subprocess.run([str(exe)] + CASES, check=True,
                             capture_output=True, text=True).stdout
    got = dict(zip(CASES, out.split("\n")))

    errors = []
    for label, shown in got.items():
        print("%-28r -> %r" % (label, shown))
        if len(label) <= width:
            if shown != label:
                errors.append("%r fits but was changed to %r" % (label, shown))
        elif len(shown) != width or "..." not in shown:
            errors.append("%r -> %r: not exactly %d chars with an ellipsis"
                          % (label, shown, width))
        elif not label.endswith(shown.split("...")[-1]) \
                or not label.startswith(shown.split("...")[0]):
            errors.append("%r -> %r: head or tail not kept" % (label, shown))

    a, b = got["Mission Pack by Rino"], got["Mission Pack by Rino 2"]
    if a == b:
        errors.append("the #136 pair still collides: both %r" % a)

    for e in errors:
        print("FAIL: " + e)
    if errors:
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
