#!/usr/bin/env python3
"""A menu keeps the pointer free; nothing takes it back behind the menu's back (#91).

Relative mouse mode stops SDL tracking the pointer, and the menus read the
pointer for hover.  Opening a menu releases it; leaving one takes it back.
Two things took it back while a menu was still up, so hover was dead:

  - the paths that re-grab on their own (window focus regained, unpause,
    _enable_mouse changed) called IN_ActivateMouse unconditionally, so the
    window's first focus or an alt-tab back captured the pointer;
  - M_Options_Draw called IN_ActivateMouse every frame, which killed hover in
    Options and every submenu reached from it.

Now the menu opening or closing decides: Key_SetDest calls
IN_MenuPointerFollowDest on a transition into or out of key_menu, and the
capture-on-their-own paths go through IN_ReactivateMouse.  A menu holds the
pointer only with ui_mouse on; with it off the mouse stays captured, which is
what lets Key Setup bind a mouse button.

Checked here:
  1. IN_MenuOwnsPointer, IN_ReactivateMouse, IN_MenuPointerFollowDest and the
     real Key_SetDest, compiled against stubs, driven through menu open/close,
     binding mode, console, and ui_mouse off.
  2. Source: focus, unpause, _enable_mouse, video restart and the borderless
     toggle never capture under a menu, and no menu draw function grabs.
"""

from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
IN_SDL = ROOT / "engine/h2shared/in_sdl.c"
KEYS_H = ROOT / "engine/h2shared/keys.h"
MENU = ROOT / "engine/hexen2/menu.c"
VIDS = [ROOT / "engine/h2shared/gl_vidsdl.c", ROOT / "engine/h2shared/vid_soft_web.c"]


def strip_comments(text):
    return re.sub(r"/\*.*?\*/", " ", re.sub(r"//[^\n]*", " ", text), flags=re.S)


def function(text, signature):
    """The definition of signature, skipping any forward declaration."""
    start = text.index(signature)
    while ";" in text[start:text.index("{", start)]:
        start = text.index(signature, start + 1)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def define(text, name):
    m = re.search(r"#define\s+%s\s+(\([^)]*\)|\S+)" % name, text)
    if not m:
        sys.exit(f"FAIL: cannot find #define {name} in keys.h")
    return m.group(1)


errors = []
in_sdl = IN_SDL.read_text()
keys = KEYS_H.read_text()

# ---------------------------------------------------------------- 1. compiled
KEYS_C = ROOT / "engine/hexen2/keys.c"
DESTS = {n: define(keys, n) for n in ("key_game", "key_console", "key_message", "key_menu")}
DESTS["key_bindbit"] = define(keys, "key_bindbit")
SRC = "".join(f"#define {k} {v}\n" for k, v in DESTS.items()) + r'''
#include <assert.h>
#include <stdio.h>
#define key_menubind (key_menu | key_bindbit)
typedef int keydest_t;
typedef int qboolean;
static struct { int integer; } ui_mouse = { 1 };
static keydest_t key_dest;
static int captured, grabs, releases;
static qboolean key_gamekey, prev_gamekey;
static keydest_t Key_GetDest (void) { return key_dest; }
static void IN_ActivateMouse (void) { grabs++; captured = 1; }
static void IN_DeactivateMouse (void) { releases++; captured = 0; }
static void Key_UpdateTextInputMode (void) {}
static qboolean Key_IsGameKey (void) { return 0; }
static void Key_ClearStates (void) {}
''' + "\n".join(function(in_sdl, sig) for sig in (
    "qboolean IN_MenuOwnsPointer (void)",
    "void IN_ReactivateMouse (void)",
    "void IN_MenuPointerFollowDest (void)")) + "\n" + \
    function(KEYS_C.read_text(), "void Key_SetDest (keydest_t dest)") + r'''
int main (void)
{
	/* In game, the mouse is captured. */
	key_dest = key_game; captured = 1;

	/* A menu opening releases it -- by keybind too (save, load, quit). */
	Key_SetDest (key_menu);		assert (!captured);
	/* Focus regained, unpause, _enable_mouse: not under the menu. */
	IN_ReactivateMouse ();		assert (!captured);
	/* Binding mode is still the menu. */
	Key_SetDest (key_menubind);	assert (!captured);
	Key_SetDest (key_menu);		assert (!captured);
	/* Closing it takes the mouse back, whichever exit does it. */
	Key_SetDest (key_game);		assert (captured);
	/* In game the self-initiated paths capture. */
	IN_DeactivateMouse ();
	IN_ReactivateMouse ();		assert (captured);
	/* Console and chat are not menus: no transition, no change. */
	grabs = releases = 0;
	Key_SetDest (key_console);	assert (captured && !grabs && !releases);
	Key_SetDest (key_game);		assert (captured && !grabs && !releases);
	/* Menu over the console, and back to the console: captured again. */
	Key_SetDest (key_menu);		assert (!captured);
	Key_SetDest (key_console);	assert (captured);

	/* ui_mouse off: the menus take no pointer input, and Key Setup binds a
	 * mouse button only with the mouse captured, so a menu keeps it. */
	ui_mouse.integer = 0;
	Key_SetDest (key_game);
	Key_SetDest (key_menu);		assert (captured);
	IN_DeactivateMouse ();
	IN_ReactivateMouse ();		assert (captured);
	Key_SetDest (key_menubind);	assert (captured);

	puts ("ok   a menu holds the pointer while open (with ui_mouse), the game takes it back");
	return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c, exe = Path(tmp) / "t.c", Path(tmp) / "t"
    c.write_text(SRC)
    r = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-Wall", "-Werror",
                        "-Wno-unused-function", "-Wno-unused-variable", "-o", str(exe), str(c)],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAIL compiling the pointer-ownership code:\n" + r.stderr)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode:
        sys.exit("FAIL pointer ownership:\n" + r.stderr)

# ---------------------------------------------------------------- 2. source
src = strip_comments(in_sdl)
m = re.search(r"case\s+SDL_EVENT_WINDOW_FOCUS_GAINED\s*:(.*?)break\s*;", src, re.S)
if not m:
    errors.append("in_sdl.c: no SDL_EVENT_WINDOW_FOCUS_GAINED case")
elif "IN_ReactivateMouse" not in m.group(1) or re.search(r"\bIN_ActivateMouse\b", m.group(1)):
    errors.append("in_sdl.c: FOCUS_GAINED must take the mouse back through IN_ReactivateMouse")

for path in VIDS:
    text = strip_comments(path.read_text())
    pause = function(text, "void VID_HandlePause (qboolean paused)")
    if re.search(r"\bIN_ActivateMouse\b", pause) or "IN_ReactivateMouse" not in pause:
        errors.append(f"{path.relative_to(ROOT)}: VID_HandlePause must unpause through IN_ReactivateMouse")
    # The _enable_mouse watcher (GL only): the Options menu flips the cvar.
    w = re.search(r"if\s*\(\s*_enable_mouse\.integer\s*!=\s*enable_mouse[^)]*\)\s*\{(.*?)\}", text, re.S)
    if w and (re.search(r"\bIN_ActivateMouse\b", w.group(1)) or "IN_ReactivateMouse" not in w.group(1)):
        errors.append(f"{path.relative_to(ROOT)}: the _enable_mouse watcher must use IN_ReactivateMouse")

# Video restart and the borderless toggle used to capture under the menu.
startup = function(src, "static void IN_StartupMouse (void)")
if not re.search(r"_enable_mouse\.integer\s*&&\s*!\s*IN_MenuOwnsPointer\s*\(\s*\)", startup):
    errors.append("in_sdl.c: IN_StartupMouse must not capture under a menu that holds the pointer")
for path in VIDS:
    if re.search(r"\bIN_ActivateMouse\b", strip_comments(path.read_text())):
        errors.append(f"{path.relative_to(ROOT)}: captures with IN_ActivateMouse; use IN_ReactivateMouse")

menu = strip_comments(MENU.read_text())
# Reached from a submenu with no Key_SetDest transition: with ui_mouse off it
# must not release, or Key Setup can no longer bind a mouse button.
main = function(menu, "void M_Menu_Main_f (void)")
if not re.search(r"if\s*\(\s*IN_MenuOwnsPointer\s*\(\s*\)\s*\)\s*IN_DeactivateMouse", main):
    errors.append("menu.c: M_Menu_Main_f must release the mouse only when the menu holds the pointer")
for name in re.findall(r"static void (M_\w+_Draw)\s*\(", menu):
    body = function(menu, f"static void {name} (")
    if re.search(r"\bIN_ActivateMouse\b", body):
        errors.append(f"menu.c: {name} grabs the mouse, which kills hover while it is up")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("ok   focus, unpause and _enable_mouse re-grab through IN_ReactivateMouse; no menu draw grabs")
print("PASS a menu keeps the pointer free (#91)")
