#!/usr/bin/env python3
"""A self-only misc_modelpimp orb (spawnflags 4 + 16) draws once, at its
controller (GitHub #518).

Inky's controllers have effects exactly EF_NODRAW, which the server never
sends, so the client has no entity for them: before #518 the orb sat on the
controller's override and nothing ever drew it.  PF_pimpmodel now marks the
override (self_orb, origin, model) and registers it with R_AddPimpSelfOrb, and
R_DrawAllGlows draws every registered orb the visedict pass did not already
draw.

This lifts the production code verbatim and runs it against stubs:

  - from engine/h2shared/pr_cmds.c, the self_orb assignment, evaluated for
    every spawnflags value 0..127 with the file's own PIMP_SF_* bits: true
    exactly when both 4 and 16 are set, so a shared orb (4 alone) adds
    nothing at the controller and SoT maps draw as before;
  - from engine/hexen2/gl_rmain.c, the override table with the self-orb list,
    R_ClearPimpOverrides, R_AddPimpSelfOrb and R_DrawAllGlows, with R_DrawGlow
    stubbed to record what it was asked to draw.

The live, end-to-end check (needs Storm over Thyrion) is
tools/pimp-orb-check.sh.  An optional argument names another gl_rmain.c, for
showing the test fails without the fix.
"""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PR_CMDS = ROOT / "engine/h2shared/pr_cmds.c"
GL_RMAIN = ROOT / "engine/hexen2/gl_rmain.c"
HEADERS = [ROOT / "engine/h2shared/gl_model.h", ROOT / "engine/h2shared/model.h"]


def fail(msg):
    sys.exit("FAIL: " + msg)


def between(text, start, end, what):
    a = text.find(start)
    if a < 0:
        fail("%s: cannot find %r" % (what, start))
    b = text.find(end, a)
    if b < 0:
        fail("%s: cannot find %r after %r" % (what, end, start))
    return text[a:b]


def function(text, signature, what):
    a = text.find(signature)
    if a < 0:
        fail("%s: cannot find %r" % (what, signature))
    b = text.find("\n}\n", a)
    return text[a:b + 3]


PRELUDE = r'''
#include <stdio.h>
#include <string.h>
typedef unsigned char byte;
typedef float vec3_t[3];
typedef int qboolean;
#define true 1
#define false 0
#define MAX_EDICTS 4096
#define VectorCopy(a,b) ((b)[0]=(a)[0],(b)[1]=(a)[1],(b)[2]=(a)[2])
#define EF_NODRAW 0x80
#define GL_ONE 1
#define GL_SRC_ALPHA 2
#define GL_ONE_MINUS_SRC_ALPHA 3
typedef enum { mod_brush, mod_sprite, mod_alias } modtype_t;
typedef struct qmodel_s { modtype_t type; } qmodel_t;
typedef struct { qmodel_t *model; vec3_t origin; int effects; } entity_t;
typedef struct {
	qboolean active;
	int ex_flags;
	qboolean self_orb;
	vec3_t origin;
	struct qmodel_s *model;
} pimp_override_t;
typedef struct { int integer; } cvar_t;
static cvar_t gl_glows = {1}, gl_missile_glows = {1}, gl_other_glows = {1}, r_drawentities = {1};
static entity_t cl_entities[MAX_EDICTS];
static entity_t *cl_visedicts[MAX_EDICTS];
static int cl_numvisedicts;
static int r_numglowinfo;
static int ndprint;
static void Con_DPrintf (const char *fmt, ...) { (void)fmt; ndprint++; }
static void R_SetDepthMask (int x) { (void)x; }
static void R_SetBlend (int x) { (void)x; }
static void R_SetBlendFunc (int a, int b) { (void)a; (void)b; }
static int ndraw;
static int draw_ent[16];
static float draw_org[16][3];
static qmodel_t *draw_model[16];
static void R_DrawGlow (entity_t *e, int entnum)
{
	if (ndraw < 16) {
		draw_ent[ndraw] = entnum;
		VectorCopy (e->origin, draw_org[ndraw]);
		draw_model[ndraw] = e->model;
	}
	ndraw++;
}
'''

DRIVER = r'''
static qmodel_t alias = { mod_alias }, brush = { mod_brush };

static void frame (const char *name)
{
	int i;
	ndraw = 0;
	R_DrawAllGlows ();
	printf ("%s %d", name, ndraw);
	for (i = 0; i < ndraw && i < 16; i++)
		printf (" %d@%.0f,%.0f,%.0f%s", draw_ent[i], draw_org[i][0],
			draw_org[i][1], draw_org[i][2],
			/* "!": not the model the controller named */
			draw_model[i] == pimp_overrides[draw_ent[i]].model ? "" : "!");
	printf ("\n");
}

/* What PF_pimpmodel leaves behind for a controller at org. */
static void pimp (int n, int self_orb, float x, float y, float z, qmodel_t *m)
{
	pimp_override_t *p = &pimp_overrides[n];
	p->active = true;
	p->self_orb = self_orb;
	p->origin[0] = x; p->origin[1] = y; p->origin[2] = z;
	p->model = m;
	if (self_orb)
		R_AddPimpSelfOrb (n);
}

int main (void)
{
	int n;

	R_ClearPimpOverrides ();
	R_AddPimpSelfOrb (-1);			/* out of range: ignored */
	R_AddPimpSelfOrb (MAX_EDICTS);
	frame ("empty");

	pimp (10, true, 1, 2, 3, &alias);
	frame ("controller");

	pimp (10, true, 1, 2, 3, &alias);	/* re-pimped every wait */
	pimp (10, true, 1, 2, 3, &alias);
	frame ("repimped");

	pimp (11, false, 4, 5, 6, &alias);	/* spawnflags 4: shared */
	frame ("shared");

	/* A visible entity pimped 4+16 is in cl_visedicts and draws there. */
	pimp (12, true, 7, 8, 9, &alias);
	cl_entities[12].model = &alias;
	cl_entities[12].origin[0] = 70;
	cl_visedicts[cl_numvisedicts++] = &cl_entities[12];
	frame ("visible");
	cl_numvisedicts = 0;
	pimp_overrides[12].active = false;

	pimp_overrides[10].active = false;	/* ED_Free */
	frame ("freed");
	pimp_overrides[10].active = true;	/* slot back, not re-pimped yet */
	frame ("stale");
	pimp (10, true, 1, 2, 3, &alias);
	frame ("again");

	pimp (10, false, 1, 2, 3, &alias);	/* re-pimped as shared */
	frame ("unflagged");
	pimp (10, true, 1, 2, 3, &alias);

	pimp (13, true, 0, 0, 0, &brush);
	frame ("brush");
	pimp_overrides[13].active = false;

	gl_glows.integer = gl_missile_glows.integer = gl_other_glows.integer = 0;
	frame ("glowsoff");
	gl_glows.integer = gl_missile_glows.integer = gl_other_glows.integer = 1;
	r_drawentities.integer = 0;
	frame ("noents");
	r_drawentities.integer = 1;

	/* R_NewMap: the next map's edict 10 is set up by hand, not through
	 * R_AddPimpSelfOrb, so only a list left over from this map could draw it. */
	R_ClearPimpOverrides ();
	pimp_overrides[10].active = true;
	pimp_overrides[10].self_orb = true;
	pimp_overrides[10].model = &alias;
	frame ("newmap");

	/* Overflow: the list keeps MAX_PIMP_SELF_ORBS and says so once. */
	R_ClearPimpOverrides ();
	for (n = 1; n <= MAX_PIMP_SELF_ORBS + 2; n++)
		R_AddPimpSelfOrb (n);
	printf ("overflow %d %d\n", num_pimp_self_orbs, ndprint);
	return 0;
}
'''

# name -> what R_DrawGlow must have been asked to draw
WANT = {
    "empty": [],
    "controller": ["10@1,2,3"],
    "repimped": ["10@1,2,3"],
    "shared": ["10@1,2,3"],
    "visible": ["12@70,0,0", "10@1,2,3"],
    "freed": [],
    "stale": [],
    "again": ["10@1,2,3"],
    "unflagged": [],
    "brush": ["10@1,2,3"],
    "glowsoff": [],
    "noents": [],
    "newmap": [],
}


def compile_and_run(program, work, name):
    c = Path(work) / (name + ".c")
    exe = Path(work) / name
    c.write_text(program)
    r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall",
                        "-Wno-unused-function", "-o", str(exe), str(c)],
                       capture_output=True, text=True)
    if r.returncode:
        fail("the lifted code no longer compiles against the stubs:\n"
             + r.stderr[-3000:])
    return subprocess.run([str(exe)], check=True, capture_output=True,
                          text=True).stdout


def check_spawnflags(work, errors):
    text = PR_CMDS.read_text()
    fn = between(text, "static void PF_pimpmodel (void)", "\n}\n", "PF_pimpmodel")
    m = re.search(r"pimp->self_orb\s*=.*?;", fn, re.S)
    if not m:
        fail("PF_pimpmodel no longer assigns pimp->self_orb")
    stmt = m.group(0)
    for needle, why in (
            (r"VectorCopy\s*\(\s*ref_ent->v\.origin\s*,\s*pimp->origin\s*\)",
             "copy the controller's origin into the override"),
            (r"pimp->model\s*=\s*mod\s*;", "record the controller's model"),
            (r"if\s*\(\s*pimp->self_orb\s*\)\s*R_AddPimpSelfOrb\s*\(\s*entnum\s*\)",
             "register a self-only orb with the renderer")):
        if not re.search(needle, fn):
            errors.append("PF_pimpmodel must " + why)
    defines = "\n".join(re.findall(r"^#define\s+PIMP_SF_\w+\s+\d+", text, re.M))
    program = "\n".join([
        "#include <stdio.h>", defines,
        "typedef struct { int self_orb; } pimp_override_t;",
        "int main (void) {",
        "\tpimp_override_t p, *pimp = &p; int spawnflags;",
        "\tfor (spawnflags = 0; spawnflags < 128; spawnflags++) {",
        "\t\t" + stmt,
        "\t\tprintf (\"%d %d\\n\", spawnflags, pimp->self_orb ? 1 : 0);",
        "\t}",
        "\treturn 0;", "}"])
    bad = []
    for line in compile_and_run(program, work, "spawnflags").split("\n"):
        if not line:
            continue
        sf, got = map(int, line.split())
        want = 1 if (sf & 4 and sf & 16) else 0
        if got != want:
            bad.append(sf)
    if bad:
        errors.append("self_orb wrong for spawnflags %s (want: exactly when "
                      "4 and 16 are both set)" % bad[:12])
    else:
        print("spawnflags 0..127: self_orb exactly when 4 and 16 are set")


def check_glow_pass(src, work, errors):
    text = src.read_text()
    table = between(text, "static pimp_override_t\tpimp_overrides[MAX_EDICTS];",
                    "pimp_override_t *R_GetPimpOverride", "the override table")
    if "R_AddPimpSelfOrb" not in table:
        errors.append("gl_rmain.c defines no R_AddPimpSelfOrb beside the override table")
        return
    pass_ = function(text, "static void R_DrawAllGlows (void)", "R_DrawAllGlows")
    out = compile_and_run(PRELUDE + table + pass_ + DRIVER, work, "glows")
    seen = {}
    if "overflow " not in out:
        errors.append("the overflow case printed nothing")
    for line in out.split("\n"):
        if line.startswith("overflow "):
            kept, said = map(int, line.split()[1:])
            ok = kept == 1024 and said == 1
            print("overflow   kept %d, said so %d time(s), want 1024 and 1  %s"
                  % (kept, said, "ok" if ok else "WRONG"))
            if not ok:
                errors.append("R_AddPimpSelfOrb overflow: kept %d, %d message(s)"
                              % (kept, said))
        elif line:
            parts = line.split()
            seen[parts[0]] = (int(parts[1]), parts[2:])
    for name, want in WANT.items():
        n, got = seen.get(name, (-1, []))
        mark = "ok" if (n == len(want) and got == want) else "WRONG"
        print("%-10s drew %s, want %s  %s" % (name, got or "nothing",
                                             want or "nothing", mark))
        if mark != "ok":
            errors.append("%s: R_DrawAllGlows drew %s, want %s"
                          % (name, got or "nothing", want or "nothing"))


SELECT_PRELUDE = r"""
#include <stdio.h>
#include <string.h>
typedef int qboolean;
typedef float vec3_t[3];
#define true 1
#define false 0
#define MAX_EDICTS 64
#define EF_GLOW (1 << 6)
#define XF_TORCH_GLOW (1 << 0)
#define XF_GLOW (1 << 1)
#define XF_MISSILE_GLOW (1 << 2)
typedef struct qmodel_s { int ex_flags; float glow_settings[10]; } qmodel_t;
typedef struct { qmodel_t *model; vec3_t origin; int effects; } entity_t;
typedef struct { qboolean active; int ex_flags; float glow_settings[10]; } pimp_override_t;
static pimp_override_t pimp_overrides[MAX_EDICTS];
static entity_t cl_entities[MAX_EDICTS];
"""

SELECT_DRIVER = r"""
int main (void)
{
	static qmodel_t plain = { 0 }, engine_glow = { XF_GLOW };
	entity_t orb;	/* on the stack, as R_DrawAllGlows builds it */
	float *gs;
	int flags;

	memset (&orb, 0, sizeof(orb));
	orb.model = &plain;
	pimp_overrides[10].active = true;
	pimp_overrides[10].ex_flags = EF_GLOW;
	select_glow (&orb, 10, &gs, &flags);
	printf ("stack %d %d\n", gs == pimp_overrides[10].glow_settings, !!(flags & EF_GLOW));

	cl_entities[11].model = &plain;	/* no override: the model's own */
	select_glow (&cl_entities[11], 11, &gs, &flags);
	printf ("plain %d %d\n", gs == plain.glow_settings, !!(flags & EF_GLOW));

	/* An engine glow the override did not ask for keeps the model's colour. */
	cl_entities[12].model = &engine_glow;
	pimp_overrides[12].active = true;
	select_glow (&cl_entities[12], 12, &gs, &flags);
	printf ("engine %d %d\n", gs == engine_glow.glow_settings, !!(flags & XF_GLOW));
	return 0;
}
"""


def check_glow_select(src, work, errors):
    """R_DrawGlow picks its override by the entnum it is handed.  The
    self-only orb is a stack entity_t, so e - cl_entities is garbage there and
    would select no override: every 4+16 orb would vanish."""
    text = src.read_text()
    fn = function(text, "static void R_DrawGlow (entity_t *e, int entnum)", "R_DrawGlow")
    if re.search(r"\be\s*-\s*cl_entities\b", fn):
        errors.append("R_DrawGlow computes e - cl_entities; it must use the "
                      "entnum it is passed (a self-only orb has no client entity)")
    head = between(fn, "static void R_DrawGlow (entity_t *e, int entnum)",
                   "// Torches & Flames", "R_DrawGlow's flag selection")
    head = head.replace(
        "static void R_DrawGlow (entity_t *e, int entnum)",
        "static void select_glow (entity_t *e, int entnum, float **gs_out, int *flags_out)")
    program = (SELECT_PRELUDE + head
               + "\t*gs_out = gsettings;\n\t*flags_out = glow_flags;\n}\n"
               + SELECT_DRIVER)
    out = compile_and_run(program, work, "select")
    want = {"stack": "1 1", "plain": "1 0", "engine": "1 1"}
    got = dict(l.split(" ", 1) for l in out.split("\n") if l)
    for k, w in want.items():
        ok = got.get(k) == w
        print("select %-7s %s, want %s  %s" % (k, got.get(k), w, "ok" if ok else "WRONG"))
        if not ok:
            errors.append("R_DrawGlow selection, %s case: got %s, want %s "
                          "(settings from the right table, flag present)"
                          % (k, got.get(k), w))


def check_headers(errors):
    # cl_main.c and pr_cmds.c build against gl_model.h on GL and model.h on
    # the software renderer; the two copies of the struct must both carry it.
    for h in HEADERS:
        body = between(h.read_text(), "// Per-entity PimpModel overrides",
                       "} pimp_override_t;", h.name)
        for field in (r"qboolean\s+self_orb;", r"vec3_t\s+origin;",
                      r"struct qmodel_s\s*\*\s*model;"):
            if not re.search(field, body):
                errors.append("%s: pimp_override_t lacks %s" % (h.name, field))
        if "void R_AddPimpSelfOrb (int entnum);" not in h.read_text():
            errors.append("%s does not declare R_AddPimpSelfOrb" % h.name)
    soft = (ROOT / "engine/hexen2/r_soft_web.c").read_text()
    if not re.search(r"void R_AddPimpSelfOrb \(int entnum\)\s*\{", soft):
        errors.append("r_soft_web.c defines no R_AddPimpSelfOrb: WEBSOFT will not link")


def main():
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else GL_RMAIN
    errors = []
    with tempfile.TemporaryDirectory() as work:
        check_spawnflags(work, errors)
        check_glow_pass(src, work, errors)
        check_glow_select(src, work, errors)
    check_headers(errors)
    for e in errors:
        print("FAIL: " + e)
    if errors:
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
