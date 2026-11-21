#!/usr/bin/env python3
"""jsh2colour gives the Hexen II fire lights their orange colour, and prints
its own name in the usage line (sezero/uhexen2#80, by James Le Cuirot).

LoadEntities in utils/jsh2color/entities.c colours a light orange
(255 128 64) by its classname: every light_flame* and light_torch*, plus
light_gem, light_candle and light_lantern.  Any other light starts uncoloured
and gets its colour from the textures near it.  Before #80, the Portals
candles (332 in tibet1-9) and the lanterns (20, in keep2, keep4, tibet2 and
HexenWorld's hwdm2) were lit by texture colour only, although the gamecode
spawns them as flames (gamecode/hc/portals/torch.hc, gamecode/hc/hw/torch.hc).

light_globe stays out on purpose: no retail map has one, and its editor
definition is commented out in the gamecode (QUAK-ED).

The test does not keep a copy of the rule in Python.  It lifts the
classname test verbatim out of entities.c and the usage printf out of
tyrlite.c, compiles each against small stubs, and runs them:

  - each classname below must come out orange or uncoloured as listed, in any
    letter case;
  - each fixed name in the orange list must be a light the gamecode spawns;
  - the usage line must begin "Usage: <argv[0]> [" for any argv[0], with the
    second line's options under the first line's.

  test_jsh2colour_orange.py [--entities FILE] [--tyrlite FILE]

Both options read another copy of the source, to show the test failing on
one from before the change.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENTITIES = ROOT / "utils/jsh2color/entities.c"
TYRLITE = ROOT / "utils/jsh2color/tyrlite.c"
GAMECODE = ROOT / "gamecode/hc"

ORANGE = (255, 128, 64)
# classname -> should it be orange?
CASES = {
    "light_candle": True,
    "light_lantern": True,
    "LIGHT_Candle": True,
    "Light_Lantern": True,
    "light_gem": True,
    "light_flame_large_yellow": True,
    "light_flame_small_yellow": True,
    "light_torch_castle": True,
    "light_torch_walltorch": True,
    "light": False,
    "light_globe": False,
    "light_burner": False,
    "light_candles": False,
    "light_lanterns": False,
    "light_cand": False,
}
MUST_LIST = ("light_candle", "light_lantern")

CLASSIFY_PRELUDE = r'''
#include <stdio.h>
#include <strings.h>
#define q_strcasecmp strcasecmp
#define q_strncasecmp strncasecmp
typedef struct { const char *classname; int lightcolor[3]; } entity_t;
static int num_clights;
static void classify (entity_t *entity)
{
'''
CLASSIFY_MAIN = r'''
}
int main (int argc, char **argv)
{
	int i;
	for (i = 1; i < argc; i++)
	{
		entity_t e = { argv[i], { -1, -1, -1 } };
		classify (&e);
		printf ("%s %d %d %d\n", argv[i], e.lightcolor[0], e.lightcolor[1], e.lightcolor[2]);
	}
	return num_clights < 0;
}
'''
USAGE_PRELUDE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main (int argc, char **argv)
{
	(void) argc;
'''
USAGE_MAIN = r'''
	return 1;
}
'''


def fail(msg):
    print("FAIL: " + msg)
    sys.exit(1)


def lift(text, start, end, what):
    a = text.find(start)
    if a < 0:
        fail("%s: could not find %r" % (what, start))
    b = text.find(end, a)
    if b < 0:
        fail("%s: could not find %r after %r" % (what, end, start))
    return text[a:b]


def build(tmp, name, source):
    c = Path(tmp) / (name + ".c")
    exe = Path(tmp) / name
    c.write_text(source)
    r = subprocess.run(["cc", "-std=c99", "-Wall", "-Werror", "-o", str(exe), str(c)],
                       capture_output=True, text=True)
    if r.returncode:
        fail("%s did not compile:\n%s" % (name, r.stderr))
    return exe


def spawned_lights():
    names = set()
    for hc in GAMECODE.rglob("*.hc"):
        text = hc.read_text(errors="replace")
        names.update(re.findall(r"^\s*void\s*(?:\(\s*\)\s*)?(light_\w+)\s*(?:\(|=)", text, re.M))
    return names


def check_classify(src, tmp):
    text = src.read_text()
    block = lift(text, "if (!q_strncasecmp(entity->classname, \"light_flame\"",
                 "if (entity->targetname[0]", "entities.c")
    errors = []

    fixed = re.findall(r'q_strcasecmp\s*\(\s*entity->classname\s*,\s*"([^"]+)"\s*\)', block)
    for name in MUST_LIST:
        if name not in fixed:
            errors.append("%s is not in the orange classname list" % name)
    lights = spawned_lights()
    for name in fixed:
        if name not in lights:
            errors.append("orange list names %s, which no gamecode spawns" % name)
    for pre, n in re.findall(r'q_strncasecmp\s*\(\s*entity->classname\s*,\s*"([^"]+)"\s*,\s*(\d+)\s*\)', block):
        if len(pre) != int(n):
            errors.append("prefix %r compared over %s characters, not %d" % (pre, n, len(pre)))

    exe = build(tmp, "classify", CLASSIFY_PRELUDE + block + CLASSIFY_MAIN)
    out = subprocess.run([str(exe)] + list(CASES), capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        name, *rgb = line.split()
        rgb = tuple(int(c) for c in rgb)
        want = ORANGE if CASES[name] else (0, 0, 0)
        if rgb != want:
            errors.append("%s comes out %s, want %s" % (name, rgb, want))
    print("classname rule: %d classnames run, orange list %s" % (len(CASES), ", ".join(fixed)))
    return errors


def check_usage(src, tmp):
    block = lift(src.read_text(), "printf (\"Usage:", "exit(0);", "tyrlite.c")
    exe = build(tmp, "usage", USAGE_PRELUDE + block + USAGE_MAIN)
    errors = []
    for argv0 in ("jsh2colour", "./jsh2colour", "/nix/store/x-utils/bin/jsh2colour"):
        # executable= runs the stub with argv[0] set to the name under test
        out =subprocess.run([argv0], executable=str(exe), capture_output=True, text=True).stdout
        lines = out.splitlines()
        if len(lines) != 2:
            errors.append("usage for %r is %d lines, want 2: %r" % (argv0, len(lines), out))
            continue
        head = "Usage: %s [" % argv0
        if not lines[0].startswith(head):
            errors.append("usage line for argv[0] %r reads %r" % (argv0, lines[0]))
            continue
        if "\t" in lines[1] or lines[1].index("[") != len(head) - 1:
            errors.append("second usage line for %r not under the first line's options: %r"
                          % (argv0, lines[1]))
    print("usage line: 3 argv[0] values run")
    return errors


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--entities", type=Path, default=ENTITIES)
    ap.add_argument("--tyrlite", type=Path, default=TYRLITE)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        errors = check_classify(args.entities, tmp) + check_usage(args.tyrlite, tmp)
    if errors:
        for e in errors:
            print("FAIL: " + e)
        sys.exit(1)
    print("PASS: light_candle and light_lantern are orange; usage names argv[0]")


if __name__ == "__main__":
    main()
