#!/usr/bin/env python3
"""engine/cmake/EmbedSpirv.cmake embeds exactly the SPIR-V the SDL_GPU
backend loads (GitHub #340, decision 6).

USE_SDL_GPU builds link <build>/engine/generated/spirv_gen.c, which
EmbedSpirv.cmake writes from <build>/engine/generated/spirv/*.vk.spv: one
uint32_t array per row of engine/h2shared/gl_progid_refl.h plus the rg_spirv[]
table rg_shader.c looks blobs up in by R_Program_Blob's name.  A blob missing
from that table is a program -gpu cannot build, found only at run time on the
first frame that wants it.  So this runs the real script against fixtures
(needs cmake and a C compiler; ci.yml runs it under `nix develop`):

  * the table has every row of the checked-in gl_progid_refl.h, sorted, and
    nothing else -- the .gl.spv twins are not embedded;
  * compiled and run with engine/h2shared/rg_spirv.h, every entry's words and
    byte size are the file's;
  * a little-endian and a big-endian file give the same words;
  * two runs write the same bytes, and no fixture path reaches the output;
  * the script FAILS on a row with no blob, a blob with no row, a size that is
    not whole words, and a file without the SPIR-V magic number.

  --generated FILE --spirv-dir DIR
      instead check a real build's spirv_gen.c against the .vk.spv beside it
      (shaders-spirv.yml, after shaders_spirv): same names, same words.

Usage: test_embed_spirv.py [--root DIR] [--generated FILE --spirv-dir DIR]
"""
from pathlib import Path
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
REFL_ROW = re.compile(r'^\s*\{ "([a-z0-9_]+\.(?:vert|frag|comp))", RPROG_REFL_', re.M)
errors = []


def fail(msg):
    errors.append(msg)


def refl_names(path):
    return REFL_ROW.findall(path.read_text())


def fake_words(name, n=12):
    """A deterministic module-shaped word stream for blob NAME."""
    seed = sum(ord(c) * (i + 1) for i, c in enumerate(name))
    words = [0x07230203, 0x00010000, 0x0008000B, 0x40 + seed % 97, 0]
    words += [(seed * 2654435761 + i * 40503) & 0xFFFFFFFF for i in range(n + seed % 5)]
    return words


def write_spv(path, words, big=False):
    path.write_bytes(struct.pack((">" if big else "<") + "%dI" % len(words), *words))


def embed(root, spv_dir, refl, out):
    r = subprocess.run(["cmake", "-DSPV_DIR=%s" % spv_dir, "-DREFL=%s" % refl,
                        "-DOUT_FILE=%s" % out, "-P",
                        str(root / "engine/cmake/EmbedSpirv.cmake")],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def parse_generated(text):
    """{name: [words]} from a spirv_gen.c, plus the table's names in order."""
    arrays = {}
    for m in re.finditer(r"static const uint32_t (\w+)\[\] = \{(.*?)\};", text, re.S):
        arrays[m.group(1)] = [int(w, 16) for w in re.findall(r"0x[0-9a-fA-F]{8}", m.group(2))]
    table = re.search(r"const rg_spirv_t rg_spirv\[\] = \{(.*?)\n\};", text, re.S)
    rows = re.findall(r'\{ "([^"]+)", (\w+), sizeof\((\w+)\) \}', table.group(1)) if table else []
    out, order = {}, []
    for name, sym, sym2 in rows:
        order.append(name)
        if sym != sym2 or sym not in arrays:
            fail("generated table: %s points at %s / sizeof(%s)" % (name, sym, sym2))
            continue
        out[name] = arrays[sym]
    return out, order


def compile_and_dump(root, gen, work):
    """Build the generated file with rg_spirv.h and dump what C sees."""
    main = work / "dump.c"
    main.write_text(
        '#include <stdio.h>\n#include "rg_spirv.h"\n'
        "int main (void)\n{\n\tint i;\n\tsize_t w;\n"
        "\tfor (i = 0; i < rg_spirv_count; i++)\n\t{\n"
        '\t\tprintf ("%s %u", rg_spirv[i].name, (unsigned) rg_spirv[i].size);\n'
        "\t\tif (((size_t) rg_spirv[i].code) % 4)\n\t\t\tprintf (\" MISALIGNED\");\n"
        "\t\tfor (w = 0; w < rg_spirv[i].size / 4; w++)\n"
        '\t\t\tprintf (" %08x", (unsigned) rg_spirv[i].code[w]);\n'
        '\t\tprintf ("\\n");\n\t}\n\treturn 0;\n}\n')
    cc = os.environ.get("CC", "cc")
    exe = work / "dump"
    r = subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-DUSE_SDL_GPU",
                        "-I", str(root / "engine/h2shared"), str(gen), str(main), "-o", str(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        fail("spirv_gen.c does not compile cleanly with rg_spirv.h:\n" + r.stderr[-2000:])
        return None
    r = subprocess.run([str(exe)], capture_output=True, text=True, check=True)
    seen = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if "MISALIGNED" in parts:
            fail("rg_spirv[] entry %s is not 4-byte aligned" % parts[0])
            parts.remove("MISALIGNED")
        seen[parts[0]] = (int(parts[1]), [int(w, 16) for w in parts[2:]])
    return seen


def fixture_checks(root):
    if not shutil.which("cmake"):
        fail("cmake not on PATH -- run under `nix develop`")
        return
    names = refl_names(root / "engine/h2shared/gl_progid_refl.h")
    if len(names) < 10:
        fail("gl_progid_refl.h: only %d rows parsed" % len(names))
        return
    refl = root / "engine/h2shared/gl_progid_refl.h"
    with tempfile.TemporaryDirectory(prefix="embed-spirv-") as tmp:
        tmp = Path(tmp)
        spv = tmp / "spirv-fixture-dir"
        spv.mkdir()
        expect = {}
        for i, n in enumerate(names):
            expect[n] = fake_words(n)
            # Every third file big-endian: the words must come out the same.
            write_spv(spv / (n + ".vk.spv"), expect[n], big=(i % 3 == 1))
            write_spv(spv / (n + ".gl.spv"), [0x07230203, 0xDEADBEEF, 0, 0, 0])
        gen1, gen2 = tmp / "gen1.c", tmp / "gen2.c"
        rc, log = embed(root, spv, refl, gen1)
        if rc:
            fail("EmbedSpirv failed on a complete fixture:\n" + log[-2000:])
            return
        embed(root, spv, refl, gen2)
        b1 = gen1.read_bytes()
        if b1 != gen2.read_bytes():
            fail("EmbedSpirv is not deterministic: two runs on one input differ")
        if str(tmp).encode() in b1 or b"spirv-fixture-dir" in b1:
            fail("EmbedSpirv writes a build path into spirv_gen.c")
        got, order = parse_generated(gen1.read_text())
        if order != sorted(names):
            missing = sorted(set(names) - set(order))
            extra = sorted(set(order) - set(names))
            fail("rg_spirv[] is not gl_progid_refl.h's rows, sorted: missing %s, extra %s%s"
                 % (missing, extra, "" if missing or extra else " (order differs)"))
        for n in names:
            if n in got and got[n] != expect[n]:
                fail("rg_spirv[] %s: words differ from the .vk.spv" % n)
        seen = compile_and_dump(root, gen1, tmp)
        if seen is not None:
            if sorted(seen) != sorted(names):
                fail("compiled table has %d entries, gl_progid_refl.h %d rows"
                     % (len(seen), len(names)))
            for n, (size, words) in seen.items():
                if n in expect and (size != 4 * len(expect[n]) or words != expect[n]):
                    fail("compiled rg_spirv[] %s: size %d / words do not match the file" % (n, size))

        # Refusals.
        def refuses(label, mutate, want):
            d = tmp / ("bad-" + label)
            shutil.copytree(spv, d)
            mutate(d)
            rc, log = embed(root, d, refl, tmp / ("bad-%s.c" % label))
            if rc == 0:
                fail("EmbedSpirv accepted %s" % label)
            elif want not in log:
                fail("EmbedSpirv refused %s without naming %r:\n%s" % (label, want, log[-800:]))
        victim = names[len(names) // 2]
        refuses("a row with no blob", lambda d: (d / (victim + ".vk.spv")).unlink(), victim)
        refuses("a blob with no row",
                lambda d: write_spv(d / "zz_stray.frag.vk.spv", fake_words("zz")), "zz_stray.frag")
        refuses("a size that is not whole words",
                lambda d: (d / (victim + ".vk.spv")).write_bytes(
                    (d / (victim + ".vk.spv")).read_bytes() + b"\0\0"), victim)
        refuses("a file with no magic number",
                lambda d: write_spv(d / (victim + ".vk.spv"), [0x12345678] * 8), victim)


def generated_checks(gen, spv_dir, root):
    got, order = parse_generated(gen.read_text())
    names = refl_names(root / "engine/h2shared/gl_progid_refl.h")
    if order != sorted(names):
        fail("%s: rg_spirv[] is not gl_progid_refl.h's %d rows, sorted (has %d)"
             % (gen, len(names), len(order)))
    total = 0
    for n in order:
        f = spv_dir / (n + ".vk.spv")
        if not f.exists():
            fail("%s: no %s beside it" % (gen, f.name))
            continue
        data = f.read_bytes()
        total += len(data)
        endian = "<" if data[:4] == b"\x03\x02\x23\x07" else ">"
        words = list(struct.unpack(endian + "%dI" % (len(data) // 4), data))
        if got.get(n) != words:
            fail("%s: %s differs from %s" % (gen, n, f))
    return len(order), total


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--generated", type=Path)
    ap.add_argument("--spirv-dir", type=Path)
    a = ap.parse_args(argv)
    root = a.root.resolve()
    if a.generated or a.spirv_dir:
        if not (a.generated and a.spirv_dir):
            ap.error("--generated and --spirv-dir go together")
        n, total = generated_checks(a.generated, a.spirv_dir, root)
        what = "%s: %d blobs, %d bytes, each the .vk.spv beside it" % (a.generated, n, total)
    else:
        fixture_checks(root)
        what = ("EmbedSpirv.cmake embeds every gl_progid_refl.h row, sorted, word-exact in both "
                "byte orders, deterministically, and refuses the four broken inputs")
    for e in errors:
        print("FAIL: " + e)
    if errors:
        print("test_embed_spirv: %d failure(s)" % len(errors))
        return 1
    print("test_embed_spirv: OK, " + what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
