#!/usr/bin/env python3
"""Demo rewind: keyframes and silent replay (GitHub #76).

Ironwail rewinds by re-reading one message back, with an undo log.  Hexen
II's entity updates are deltas against a reference frame rebuilt on every
svc_reference, so re-reading an old message corrupts state; rewind here is a
keyframe of client state about once a second of demo time and a silent
forward replay from it.  Held:

  - the left arrow gives a negative speed and CL_AdvanceTime hands it to
    CL_DemoRewind; CL_GetDemoMessage never reads while speed <= 0;
  - CL_DemoRewind restores a keyframe, seeks, and replays while
    cl.time > cl.mtime[0] -- forward playback's own read condition -- with
    sound starts muted and prints, centerprints, stuffed commands and music
    changes held back (cls.demoseeking);
  - before the first keyframe it clamps there (the backstop);
  - a keyframe saves and restores the same things in the same order, and
    restore checks it consumed exactly what was saved;
  - keyframes are dropped at a level change and at the end of playback, and
    capped (thinned by halves) so a long demo stays bounded;
  - the demo bar shows "<<" while rewinding; `demoseek` exists.

Measured headless on a recorded demo1 demo (paused, particles off): seek to
40 s, away to 55 s, back to 40 s -- the frames are identical outside the sky,
which scrolls on realtime; same at 33.5 s.  Holding the left arrow 3 s went
back 15 s of demo time at ~1 ms a step.  No sound channel is picked during a
seek or rewind.  demoseek 0 freezes at the first keyframe; demoseek past the
end stops playback cleanly.  Keyframes are ~18-22 KB (zero-run encoded).
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []
d = read("engine/hexen2/cl_demo.c")

def fn(name, src=d):
    m = re.search(r"(?:static )?\w+ %s \([^)]*\)\s*\{(.*?)\n\}" % re.escape(name), src, re.S)
    return m.group(1) if m else ""

sp = fn("CL_UpdateDemoSpeed")
if "cls.demospeed = -5.0f *" not in sp:
    errors.append("the left arrow must give a negative (rewind) speed")
at = fn("CL_AdvanceTime")
if "CL_DemoRewind (cl.time + cls.demospeed * host_frametime);" not in at:
    errors.append("CL_AdvanceTime must hand a negative speed to CL_DemoRewind")
if "else if (cls.demospeed <= 0.0f)\n\t\t\treturn 0;" not in fn("CL_GetDemoMessage"):
    errors.append("CL_GetDemoMessage must not read while paused or rewinding")

rw = fn("CL_DemoRewind")
for need in ("CL_DemoKey_Restore (&demo_keys[k]);", "FS_fseek (&cls.demofh, demo_keys[k].fileofs, SEEK_SET);",
             "cls.demoseeking = true;", "snd_mute_starts++;", "while (cl.time > cl.mtime[0]",
             "CL_ReadDemoMessage ()", "snd_mute_starts--;", "cls.demoseeking = false;",
             "target = demo_keys[0].mtime;", "cl.oldtime = cl.time;"):
    if need not in rw:
        errors.append(f"CL_DemoRewind lacks {need!r}")

save, rest = fn("CL_DemoKey_Save"), fn("CL_DemoKey_Restore")
def items(body):
    out = []
    for m in re.finditer(r"DemoState_(Put|Get) \((?:s|&cur), ([^,]+),|(CL_\w+?)_(Save|Restore)State|(V)_(Save|Restore)State", body):
        if m.group(2): out.append(m.group(2).strip())
        elif m.group(3): out.append(m.group(3))
        else: out.append("V")
    return out
if items(save) != items(rest):
    errors.append("keyframe save and restore must cover the same items in the same order:\n    %s\n    %s" % (items(save), items(rest)))
if "!= k->rawsize" not in rest:
    errors.append("keyframe restore must check it consumed exactly what was saved")
for f, need in (("engine/hexen2/cl_tent.c", "void CL_TEnts_SaveState"), ("engine/hexen2/cl_effect.c", "void CL_Effects_SaveState"),
                ("engine/hexen2/view.c", "void V_SaveState")):
    if need not in read(f):
        errors.append(f"{f} lacks {need}")

if "CL_DemoKeys_Clear ();" not in read("engine/hexen2/cl_main.c") or "CL_DemoKeys_Clear ();" not in fn("CL_StopPlayback"):
    errors.append("keyframes must be dropped at a level change (CL_ClearState) and at the end of playback")
if "CL_DemoKeyframe ();" not in read("engine/hexen2/cl_main.c"):
    errors.append("CL_ReadFromServer must offer every parsed message to CL_DemoKeyframe")
kf = fn("CL_DemoKeyframe")
if "demo_numkeys == DEMO_MAX_KEYS" not in kf or "demo_keyinterval *= 2.0;" not in kf:
    errors.append("the keyframe store must be bounded (thin by halves at DEMO_MAX_KEYS)")

parse = read("engine/hexen2/cl_parse.c")
for svc in ("svc_spawnstatic", "svc_spawnstaticsound"):
    m = re.search(r"case %s:(.*?)break;" % svc, parse, re.S)
    if not m or "CL_DemoKeys_Clear ();" not in m.group(1):
        errors.append(f"{svc} mid-demo must drop the keyframes: replaying it would link statics twice")
for svc in ("svc_centerprint", "svc_stufftext", "svc_cdtrack", "svc_midi_name", "svc_print"):
    m = re.search(r"case %s:(.*?)break;" % svc, parse, re.S)
    if not m or "cls.demoseeking" not in m.group(1):
        errors.append(f"{svc} must be held back while a rewind replays")
snd = read("engine/h2shared/snd_dma.c")
if "if (nosound.integer || snd_mute_starts)" not in snd:
    errors.append("S_StartSound must honour snd_mute_starts")
if 'str = "<<";' not in read("engine/h2shared/gl_screen.c"):
    errors.append('the demo bar must show "<<" while rewinding')
if 'Cmd_AddCommand ("demoseek", CL_DemoSeek_f);' not in d:
    errors.append("demoseek must be registered")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS demo rewind: keyframes and silent replay (#76)")
