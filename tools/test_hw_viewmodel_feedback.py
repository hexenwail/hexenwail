#!/usr/bin/env python3
"""Data-free regression test for HexenWorld local presentation routing."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    text = (ROOT / path).read_text()
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    hw = (ROOT / "engine/hexen2/cl_hw.c").read_text()
    view = (ROOT / "engine/hexen2/view.c").read_text()

    # Pin every one-byte local-feedback opcode to an action rather than a
    # packet-alignment-only discard, and keep view tint / flags on viewent.
    assert "case HW_SVC_SMALLKICK:\n\t\t\tV_SetPunchAngle (-2);" in hw
    assert "case HW_SVC_BIGKICK:\n\t\t\tV_SetPunchAngle (-4);" in hw
    assert "case HW_SVC_MUZZLEFLASH:\n\t\t\tHWCL_ParseMuzzleFlash ();" in hw
    assert "cl.viewent.colorshade = MSG_ReadByte ();" in hw
    assert "cl.viewent.drawflags |= MSG_ReadByte ();" in hw
    assert "cl.viewent.drawflags &= ~MSG_ReadByte ();" in hw
    assert "V_DecayPunchAngle ();" in hw
    assert "HWCL_ViewModelVisible" in view

    prelude = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int qboolean;
#define true 1
#define false 0
#define PITCH 0
#define YAW 1
#define ROLL 2
#define STAT_WEAPON 2
#define MAX_MODELS 8
#define LERP_RESETANIM 4
typedef struct qmodel_s { int id; } qmodel_t;
typedef float vec3_t[3];
typedef struct { qmodel_t *model; int frame, effects, scale, drawflags, abslight, lerpflags; } entity_t;
typedef struct { int weaponframe, effects, scale, drawflags, abslight; } hwcl_entity_state_t;
static struct { int stats[32]; } hwcl_server_state;
static struct { qmodel_t *model_precache[MAX_MODELS]; entity_t viewent; vec3_t punchangle; double punchtime, time; } cl;
static float host_frametime;
static vec3_t v_punchangles[2];
#define VectorCopy(a,b) memcpy((b), (a), sizeof(vec3_t))
#define VectorClear(a) memset((a), 0, sizeof(vec3_t))
'''
    test = r'''
int main(void) {
    qmodel_t old = {1}, weapon = {2};
    hwcl_entity_state_t state = {7, 11, 12, 13, 14};
    cl.viewent.model = &old;
    cl.model_precache[3] = &weapon;
    hwcl_server_state.stats[STAT_WEAPON] = 3;
    HWCL_ApplyViewModel(&state);
    assert(cl.viewent.model == &weapon && cl.viewent.frame == 7);
    assert(cl.viewent.effects == 11 && cl.viewent.scale == 12);
    assert(cl.viewent.drawflags == 13 && cl.viewent.abslight == 14);
    assert(cl.viewent.lerpflags & LERP_RESETANIM);
    hwcl_server_state.stats[STAT_WEAPON] = MAX_MODELS;
    HWCL_ApplyViewModel(&state);
    assert(cl.viewent.model == NULL);

    cl.punchtime = 10;
    V_SetPunchAngle(-4);
    assert(cl.punchangle[PITCH] == -4 && v_punchangles[0][PITCH] == -4);
    host_frametime = 0.1f;
    V_DecayPunchAngle();
    assert(cl.punchangle[PITCH] == -3 && v_punchangles[1][PITCH] == -4);
    host_frametime = 1.0f;
    V_DecayPunchAngle();
    assert(cl.punchangle[PITCH] == 0 && v_punchangles[0][PITCH] == 0);
    puts("PASS: HW viewmodel state and one-shot kick routing");
    return 0;
}
'''
    source = (prelude + function("engine/hexen2/cl_hw.c",
                                 "static void HWCL_ApplyViewModel") + "\n" +
              function("engine/hexen2/view.c", "void V_SetPunchAngle") + "\n" +
              function("engine/hexen2/view.c", "void V_DecayPunchAngle") + "\n" +
              test)
    with tempfile.TemporaryDirectory(prefix="hw-viewmodel-feedback-") as tmp:
        cfile = Path(tmp) / "presentation.c"
        binary = Path(tmp) / "presentation"
        cfile.write_text(source)
        subprocess.run([
            os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
            "-Werror", str(cfile), "-o", str(binary)
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
