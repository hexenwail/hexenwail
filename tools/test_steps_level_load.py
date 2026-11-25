#!/usr/bin/env python3
"""No headless-drive.sh step file loads a level through `cmd` (#553).

`cmd TEXT` is grave, TEXT, Return, grave.  For a command that loads a level
the closing grave reaches the engine while the load is still going, and
Con_ToggleConsole_f calls SCR_EndLoadingPlaque: the loading plaque drops
before signon ends, SCR_SetUpToDrawConsole runs with con_forcedup set and
snaps the console to full height.  The console then slides back up at
scr_conspeed * host_frametime per frame.  tools/sweep-mirror-gpu.steps holds
host_framerate at 0.000001, so there it never moved again: its demo1f frames
10 - 12 were the console on every binary, at any load wait.  The other sweeps
only got away with it because their console slides off in real time; which
way the toggle fell still depended on when in the load the grave landed.

The form that has no toggle in flight: `console` then `enter map X`.
Host_Map_f closes the console itself (Key_SetDest (key_game)), and so do
load, connect and playdemo / timedemo.  changelevel, changelevel2, restart
and reconnect leave key_dest alone, so after one of those the step file
also needs a `console` once the load wait is over.

A line grep: a load reached through `type` + `key Return`, or a `cmd exec`
of a cfg that loads, is out of its reach.
"""
from pathlib import Path
import sys

# Console commands that start a level load (a loading plaque, or a signon),
# by whether they close the console themselves (Key_SetDest (key_game)).
CLOSES = {"map", "load", "connect", "playdemo", "timedemo"}
LEAVES = {"changelevel", "changelevel2", "restart", "reconnect"}
# Bare `map` / `changelevel` only print the current level.
NEEDS_ARG = {"map", "changelevel", "changelevel2", "load", "connect",
             "playdemo", "timedemo"}

tools = Path(__file__).resolve().parent
steps = sorted(tools.glob("*.steps"))
if len(sys.argv) > 1:
    steps = [Path(a) for a in sys.argv[1:]]
if not steps:
    sys.exit("FAIL: no tools/*.steps found")

errors = []
for path in steps:
    for n, line in enumerate(path.read_text().splitlines(), 1):
        words = line.split(None, 1)
        if len(words) < 2 or words[0] != "cmd":
            continue
        for sub in words[1].split(";"):
            args = sub.split()
            if not args:
                continue
            verb = args[0].lower()
            if verb in NEEDS_ARG and len(args) < 2:
                continue
            if verb in CLOSES:
                fix = f"`console` + `enter {sub.strip()}`"
            elif verb in LEAVES:
                fix = (f"`console` + `enter {sub.strip()}`, the load wait, "
                       f"then `console` ({verb} leaves the console open)")
            else:
                continue
            errors.append(f"{path.name}:{n}: `{line.strip()}` -- write {fix}")
if errors:
    sys.exit("FAIL: a level load sent with `cmd` leaves its closing console "
             "toggle in flight mid-load (#553):\n  " + "\n  ".join(errors))
print(f"PASS no level load through `cmd` in {len(steps)} step files")
