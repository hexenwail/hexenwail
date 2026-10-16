#!/usr/bin/env python3
"""misc_modelpimp spawnflag 64 ("no dynamic light") must gate every standing
light an entity casts, and must leave the muzzle flash alone (GitHub #140).

uhexen2-vyfi gated EF_BRIGHTLIGHT, EF_DIMLIGHT, EF_DARKLIGHT and EF_LIGHT on
XF_NO_DLIGHT; the torch glow and the EF_ILLUMINATE cast light were already
gated.  EF_MUZZLEFLASH is deliberately NOT gated (the comment above the block
in cl_main.c says why).  Nothing in stock content reaches the newly gated path,
and a screenshot test could not be trusted to measure it, so this checks the
code itself: the dlight block of CL_RelinkEntities -- from the
R_GetPimpFlags lookup to the trail-flag block that follows it -- is lifted out
of engine/hexen2/cl_main.c verbatim, compiled against stubs that count
CL_AllocDlight calls, and run for every light source with and without the
flag.

Expected, per source:
  - without XF_NO_DLIGHT: exactly one dlight;
  - with it: none, except EF_MUZZLEFLASH, which still gets its one.

The constants come from engine/h2shared/gl_model.h, the header the GL client
builds against.  An optional argument checks another copy of cl_main.c, for
showing the test fails with the gate removed.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CL_MAIN = ROOT / "engine/hexen2/cl_main.c"
GL_MODEL = ROOT / "engine/h2shared/gl_model.h"

START = "pflags = R_GetPimpFlags (ent, &pimp_gs);"
END = "/* Use per-entity trail flags"

CONSTANTS = ["EF_MUZZLEFLASH", "EF_BRIGHTLIGHT", "EF_DIMLIGHT", "EF_DARKLIGHT",
             "EF_LIGHT", "EF_ILLUMINATE", "XF_TORCH_GLOW", "XF_NO_DLIGHT",
             "COLOR_R", "COLOR_G", "COLOR_B", "COLOR_A", "ORB_OFFSET_X",
             "ORB_OFFSET_Y", "ORB_OFFSET_Z", "LIGHT_STYLE", "LIGHT_RADIUS"]

# (name, where the bit goes, the bit): "effects" is the entity's own
# .effects, "pflags" the model/pimp flags R_GetPimpFlags returns.
SOURCES = [
    ("EF_MUZZLEFLASH", "effects"),
    ("EF_BRIGHTLIGHT", "effects"),
    ("EF_DIMLIGHT", "effects"),
    ("EF_DARKLIGHT", "effects"),
    ("EF_LIGHT", "effects"),
    ("XF_TORCH_GLOW", "pflags"),
    ("EF_ILLUMINATE", "pflags"),
]
UNGATED = {"EF_MUZZLEFLASH"}

PRELUDE = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef float vec3_t[3];
typedef int qboolean;
#define true 1
#define false 0
#define VectorCopy(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define VectorMA(a,s,v,o) ((o)[0]=(a)[0]+(s)*(v)[0],(o)[1]=(a)[1]+(s)*(v)[1],(o)[2]=(a)[2]+(s)*(v)[2])
static void AngleVectors (vec3_t a, vec3_t f, vec3_t r, vec3_t u)
{ (void)a; memset(f,0,12); memset(r,0,12); memset(u,0,12); }
typedef struct { float origin[3]; float radius, minlight, die; float color[4]; qboolean dark; } dlight_t;
typedef struct { int effects; vec3_t origin, angles; } entity_t;
typedef struct { int integer; float value; } cvar_t;
typedef struct { int length; char map[64]; } lightstyle_t;
static struct { double time; } cl;
static lightstyle_t cl_lightstyle[64];
static cvar_t gl_missile_glows = {1, 1}, gl_flashintensity = {1, 1}, gl_torch_dlight = {1, 1};
static dlight_t dlights[32];
static int nalloc;
static dlight_t *CL_AllocDlight (int key) { (void)key; return &dlights[nalloc++ & 31]; }
static int stub_pflags;
static float stub_gs[16];
static int R_GetPimpFlags (entity_t *e, float **gs) { (void)e; *gs = stub_gs; return stub_pflags; }
'''


def constants():
    text = GL_MODEL.read_text()
    out = []
    for name in CONSTANTS:
        m = re.search(r"#define\s+%s\s+(.+?)(?:/\*|$)" % name, text, re.M)
        if not m:
            sys.exit("FAIL: %s not defined in %s" % (name, GL_MODEL))
        out.append("#define %s %s" % (name, m.group(1).strip()))
    return "\n".join(out)


def block(text):
    try:
        a = text.index(START)
        b = text.index(END, a)
    except ValueError:
        sys.exit("FAIL: could not find the dlight block (%r .. %r) in cl_main.c"
                 % (START, END))
    return text[a:b]


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else CL_MAIN
    body = block(src.read_text())
    if "nodlight" not in body:
        print("note: the block never mentions nodlight")

    cases = []
    for name, where in SOURCES:
        for nod in (0, 1):
            cases.append((name, where, nod))

    calls = []
    for name, where, nod in cases:
        eff = name if where == "effects" else "0"
        pfl = name if where == "pflags" else "0"
        calls.append("\trun (%s, (%s)%s, \"%s\", %d);"
                     % (eff, pfl, " | XF_NO_DLIGHT" if nod else "", name, nod))

    program = "\n".join([
        PRELUDE,
        constants(),
        "static void run (int effects, int model_flags, const char *name, int nod)",
        "{",
        "\tentity_t real_ent, *ent = &real_ent;",
        "\tdlight_t *dl;",
        "\tfloat *pimp_gs;",
        "\tint nodlight, pflags, i = 1;",
        "\tmemset (ent, 0, sizeof(*ent));",
        "\tent->effects = effects;",
        "\tstub_pflags = model_flags;",
        "\tnalloc = 0;",
        "\t(void)dl; (void)nodlight; (void)pimp_gs;",
        "\t" + body,
        "\tprintf (\"%s %d %d\\n\", name, nod, nalloc);",
        "}",
        "int main (void)",
        "{",
        "\n".join(calls),
        "\treturn 0;",
        "}",
    ])

    with tempfile.TemporaryDirectory() as work:
        c = Path(work) / "nodlight.c"
        exe = Path(work) / "nodlight"
        c.write_text(program)
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-w",
                            "-o", str(exe), str(c)],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("FAIL: the lifted block no longer compiles against the "
                     "stubs:\n" + r.stderr[-3000:])
        out = subprocess.run([str(exe)], check=True, capture_output=True,
                             text=True).stdout.split()

    errors = []
    for k in range(0, len(out), 3):
        name, nod, n = out[k], int(out[k + 1]), int(out[k + 2])
        want = 1 if (not nod or name in UNGATED) else 0
        flag = "with flag 64" if nod else "without     "
        print("%-15s %s -> %d dlight(s), want %d" % (name, flag, n, want))
        if n != want:
            errors.append("%s %s: %d dlights, want %d" % (name, flag.strip(), n, want))

    for e in errors:
        print("FAIL: " + e)
    if errors:
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
