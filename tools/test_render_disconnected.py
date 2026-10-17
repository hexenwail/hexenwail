#!/usr/bin/env python3
"""V_RenderView draws nothing for a client that is not connected (#204).

Host_Map_f disconnects -- shutting the local server down -- and only then
puts the loading plaque up.  SCR_SetUpToDrawConsole decides nothing while the
plaque is up, so con_forcedup kept its in-game false and V_RenderView drew
the map the client had just left, one more time, after the disconnect.  The
frame never showed it (with signon reset the plaque draws the opaque menu
backdrop over the whole screen), but on NVIDIA under offscreen EGL that draw
left the driver faulting inside the next draw call: every `map` change in a
headless run died with SIGSEGV, and older builds hung on quit instead.

The check is at source level because the failure needs that driver:
  - V_RenderView returns before R_PushDlights / R_RenderView when
    cls.state != ca_connected;
  - the plaque path still reaches the backdrop it relies on: with signon not
    at SIGNONS, SCR_UpdateScreen draws Draw_MenuBackdrop, so skipping the
    3D view changes nothing on screen;
  - demo playback is connected (CL_PlayDemo_f sets ca_connected), so the
    guard cannot blank a demo, and a demo that ends at EOF without a
    disconnect (CL_StopPlayback alone) leaves the client not signed on, so
    the console comes up rather than a HUD over an undrawn view.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def strip(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


view = strip((ROOT / "engine/hexen2/view.c").read_text())
m = re.search(r"\nvoid V_RenderView \(void\)\s*\{(.*?)\n\}", view, re.S)
if not m:
    errors.append("view.c: no V_RenderView")
else:
    body = m.group(1)
    guard = re.search(r"if\s*\(\s*cls\.state\s*!=\s*ca_connected\s*\)\s*return\s*;", body)
    render = body.find("R_RenderView")
    dlights = body.find("R_PushDlights")
    if not guard:
        errors.append("V_RenderView must return when cls.state != ca_connected")
    elif render < 0 or guard.start() > render or (dlights >= 0 and guard.start() > dlights):
        errors.append("the connection guard must come before R_PushDlights and R_RenderView")

screen = strip((ROOT / "engine/h2shared/gl_screen.c").read_text())
if not re.search(r"else if \(scr_drawloading\)\s*\{\s*if \(!cl\.worldmodel \|\| cls\.signon != SIGNONS\)\s*Draw_MenuBackdrop \(\);",
                 screen):
    errors.append("gl_screen.c: the plaque no longer covers a not-signed-on frame with Draw_MenuBackdrop; "
                  "skipping the 3D view there would now show garbage")

demo = strip((ROOT / "engine/hexen2/cl_demo.c").read_text())
play = re.search(r"\nvoid CL_PlayDemo_f \(void\)\s*\{(.*?)\n\}", demo, re.S)
if not play or "cls.state = ca_connected;" not in play.group(1):
    errors.append("CL_PlayDemo_f must run playback connected, or the guard blanks demos")
# A demo that ends at EOF goes through CL_StopPlayback alone; it must leave the
# client not signed on, or the HUD draws over the 3D view the guard skips.
stop = re.search(r"\nvoid CL_StopPlayback \(void\)\s*\{(.*?)\n\}", demo, re.S)
if not stop or not re.search(r"cls\.state = ca_disconnected;\s*cls\.signon = 0;", stop.group(1)):
    errors.append("CL_StopPlayback must reset cls.signon with cls.state, as CL_Disconnect does")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS a disconnected client renders no world; the plaque's backdrop covers that frame (#204)")
