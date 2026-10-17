#!/usr/bin/env python3
"""joy_always_active is Ironwail's: rumble and gyro act only while the pad is
the input last used, unless it is set (GitHub #409).

Upstream's IN_JoyActive gates exactly two things, rumble and gyro steering
(in_sdl.c IN_GyroMove / the rumble update); IN_JoyMove reads the sticks
regardless, so this must NOT gate movement.  Last-used tracking follows
upstream: keyboard and text input, mouse buttons and wheel (not motion),
gamepad buttons, emitted pad key events, and sticks/triggers past deadzone.
Gyro calibration keeps running whatever the gate says.
"""
from pathlib import Path
import re
import sys

src = (Path(__file__).resolve().parents[1] / "engine/h2shared/in_sdl.c").read_text()
errors = []

if 'cvar_t	joy_always_active = {"joy_always_active", "0", CVAR_ARCHIVE};' not in src:
    errors.append("joy_always_active must default to 0, archived, as upstream")
if "Cvar_RegisterVariable (&joy_always_active);" not in src:
    errors.append("joy_always_active must be registered")
ja = re.search(r"static qboolean IN_JoyActive \(void\)\s*\{(.*?)\n\}", src, re.S)
if not ja or "gp_active != NULL && (joy_always_active.integer || in_lastactive == IN_LAST_GAMEPAD)" not in ja.group(1):
    errors.append("IN_JoyActive must be: a pad, and joy_always_active or the pad was last used")

rum = re.search(r"void IN_GPRumble \([^)]*\)\s*\{(.*?)\n\}", src, re.S)
if not rum or "!IN_JoyActive ()" not in rum.group(1):
    errors.append("rumble must be gated on IN_JoyActive")
gy = re.search(r"static void IN_GyroMove \(void\)\s*\{(.*?)\n\}", src, re.S)
if not gy or "if (!IN_JoyActive ())" not in gy.group(1):
    errors.append("gyro steering must be gated on IN_JoyActive")
elif gy.group(1).index("if (!IN_JoyActive ())") < gy.group(1).index("if (gyro_calibrating)"):
    errors.append("gyro calibration must run before (regardless of) the IN_JoyActive gate")
if src.count("IN_JoyActive ()") != 2:	# calls only; declarations spell it (void)
    errors.append("IN_JoyActive must gate rumble and gyro only, not movement")

for case, kind in (("case SDL_EVENT_TEXT_INPUT:", "KEYBOARD"), ("case SDL_EVENT_KEY_UP:", "KEYBOARD"),
                   ("case SDL_EVENT_MOUSE_BUTTON_UP:", "MOUSE"), ("case SDL_EVENT_MOUSE_WHEEL:", "MOUSE"),
                   ("case SDL_EVENT_GAMEPAD_BUTTON_UP:", "GAMEPAD")):
    i = src.find(case)
    if i < 0 or f"in_lastactive = IN_LAST_{kind};" not in src[i:i+200]:
        errors.append(f"{case} must record IN_LAST_{kind}")
mm = src.find("case SDL_EVENT_MOUSE_MOTION:")
if mm >= 0 and "in_lastactive" in src[mm:mm+300]:
    errors.append("mouse motion must not count as switching device (upstream)")
if "in_lastactive = IN_LAST_GAMEPAD;	/* joy_always_active, #409 */" not in src:
    errors.append("stick/trigger deflection past deadzone must record the pad")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS joy_always_active gates rumble and gyro on last-used input, as Ironwail (#409)")
