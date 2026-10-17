#!/usr/bin/env python3
"""r_lerp_autodetect snaps the pose pairs that are too far apart, not the model (#94).

The heuristic measured every blended pose pair and, when the worst crossed
r_lerp_autodetect_threshold, set MOD_NOLERP for the whole model: one violent
death frame cost the idle, walk and run their smoothness (golem_i lost it all
over Rpound11 -> Rpound12; golem_s, 0.345 on the same animation, kept it).
Now each measured pair is recorded and only the pairs over the threshold snap.

Checked here:
  - Mod_SetExtraFlags no longer turns MOD_NOLERP on from the heuristic
    (r_nolerp_list still does, by name);
  - Mod_ComputeFlipbookRatio resets and records lerp_next / lerp_ratio per
    measured pair, in the 1/10000 units R_AliasPairSnaps reads;
  - R_AliasResolveLerp consults R_AliasPairSnaps for the pair it would blend;
  - the real R_AliasPairSnaps, compiled against a mock model: snaps a
    measured pair over the threshold in either direction, blends one under it,
    blends an unmeasured pair, and does nothing with r_lerp_autodetect 0.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


model = strip((ROOT / "engine/h2shared/gl_model.c").read_text())
extra = re.search(r"void Mod_SetExtraFlags \(qmodel_t \*mod\)\s*\{(.*?)\n\}", model, re.S)
if not extra or "r_lerp_autodetect" in extra.group(1) or "flipbook_max_ratio" in extra.group(1):
    errors.append("Mod_SetExtraFlags must not set MOD_NOLERP from the heuristic any more")
ratio = model[model.index("static void Mod_ComputeFlipbookRatio (qmodel_t *mod, aliashdr_t *hdr)"):]
ratio = ratio[:ratio.index("\n}\n")]
for want in ("Mod_ClearLerpPairs (mod);", "mod->lerp_next[a] = (short) b;", "ratio * 10000.0f"):
    if want not in ratio:
        errors.append(f"Mod_ComputeFlipbookRatio: missing {want}")
if ratio.index("Mod_ClearLerpPairs (mod);") > ratio.index("if (mod->type != mod_alias || !hdr)"):
    errors.append("Mod_ComputeFlipbookRatio must reset the pair table before any early return")
if "mod->lerp_states_only = states_only && measured > 0;" not in ratio:
    errors.append("Mod_ComputeFlipbookRatio must mark a model measured only in pass 1 (standalone states)")
load = model[model.index("static qmodel_t *Mod_LoadModel (qmodel_t *mod, qboolean crash)\n{"):]
load = load[:load.index("\n}\n")]
if not re.search(r"loadmodel = mod;\s*Mod_ClearLerpPairs \(mod\);", load):
    errors.append("Mod_LoadModel must clear the pair table before any loader (an MD3/MD5 substitute never measures)")

rmain = (ROOT / "engine/hexen2/gl_rmain.c").read_text()
if "!R_AliasPairSnaps (e->model, e->previouspose, e->currentpose) &&" not in rmain:
    errors.append("R_AliasResolveLerp must consult R_AliasPairSnaps for the pair it blends")
fn = re.search(r"static qboolean R_AliasPairSnaps \(const qmodel_t \*m, int a, int b\)\n\{.*?\n\}", rmain, re.S)
if not fn:
    errors.append("no R_AliasPairSnaps")
else:
    harness = r"""
#include <stdio.h>
typedef int qboolean;
#define false 0
#define true 1
#define MAXALIASFRAMES 256
typedef struct { float flipbook_max_ratio; short lerp_next[MAXALIASFRAMES]; unsigned short lerp_ratio[MAXALIASFRAMES]; qboolean lerp_states_only; } qmodel_t;
typedef struct { int integer; float value; } cvar_t;
static cvar_t r_lerp_autodetect = { 1, 1.0f }, r_lerp_autodetect_threshold = { 0, 0.35f };
@FN@
int main (void)
{
	static qmodel_t m;
	int i, bad = 0;
	for (i = 0; i < MAXALIASFRAMES; i++) m.lerp_next[i] = -1;
	m.lerp_next[10] = 11; m.lerp_ratio[10] = 3640;	/* golem_b's 0.364: over */
	m.lerp_next[11] = 12; m.lerp_ratio[11] = 3450;	/* 0.345: under */
	m.lerp_next[255] = 254; m.lerp_ratio[255] = 9000;	/* last pose, group wrap */
#define CHECK(c, want) do { if ((c) != (want)) { printf ("FAIL line %d\n", __LINE__); bad++; } } while (0)
	CHECK (R_AliasPairSnaps (&m, 10, 11), true);	/* the violent pair */
	CHECK (R_AliasPairSnaps (&m, 11, 10), true);	/* played backwards */
	CHECK (R_AliasPairSnaps (&m, 11, 12), false);	/* under the threshold */
	CHECK (R_AliasPairSnaps (&m, 3, 40), false);	/* never measured: blends */
	CHECK (R_AliasPairSnaps (&m, 10, 10), false);	/* no change */
	CHECK (R_AliasPairSnaps (&m, 255, 254), true);	/* top of the table */
	CHECK (R_AliasPairSnaps (&m, -1, 10), false);	/* out of range is not a crash */
	r_lerp_autodetect_threshold.value = 0.37f;
	CHECK (R_AliasPairSnaps (&m, 10, 11), false);	/* the cvar reads live */
	r_lerp_autodetect_threshold.value = 0.35f;
	r_lerp_autodetect.integer = 0;
	CHECK (R_AliasPairSnaps (&m, 10, 11), false);	/* heuristic off: list only */
	r_lerp_autodetect.integer = 1;
	/* Standalone states (no animation runs): the QC jumps anywhere, so the
	 * whole-model verdict stands -- an unmeasured jump snaps too. */
	m.lerp_states_only = true; m.flipbook_max_ratio = 0.469f;	/* barrel */
	CHECK (R_AliasPairSnaps (&m, 0, 2), true);
	m.flipbook_max_ratio = 0.2f;
	CHECK (R_AliasPairSnaps (&m, 0, 2), false);
	if (!bad) printf ("ok   R_AliasPairSnaps behaves per pair\n");
	return bad != 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "t.c", Path(tmp) / "t"
        src.write_text(harness.replace("@FN@", fn.group(0)))
        r = subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-o", str(exe), str(src)],
                           capture_output=True, text=True)
        if r.returncode:
            errors.append("R_AliasPairSnaps did not compile standalone:\n" + r.stderr[-1500:])
        else:
            r = subprocess.run([str(exe)], capture_output=True, text=True)
            sys.stdout.write(r.stdout)
            if r.returncode:
                errors.append("R_AliasPairSnaps behaves wrongly")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS r_lerp_autodetect snaps per pose pair, not per model (#94)")
