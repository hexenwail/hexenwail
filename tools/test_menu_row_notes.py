#!/usr/bin/env python3
"""The Options submenus' row notes fit the canvas and name the cvar the row sets (#98).

M_Display_Note, M_Rendering_Note and M_Graphics_Note (engine/hexen2/menu.c)
return a one-line note for a highlighted row whose name does not say what it
does.  Two ways one goes wrong without anyone noticing, both checked here:

  - longer than M_ROWNOTE_MAX characters, so it runs off the 320-unit canvas
    (M_DrawRowNote centres it);
  - describing a cvar the row does not set: each note carries the cvar as a
    trailing comment, and that cvar must appear in the row's own handler
    (a `case ROW:` block that calls Cvar_Set*) or slider table entry.  A row
    rewired to another cvar then fails here instead of keeping a stale note.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
menu = (ROOT / "engine/hexen2/menu.c").read_text()
errors = []

m = re.search(r"#define\s+M_ROWNOTE_MAX\s+(\d+)", menu)
if not m:
    sys.exit("FAIL: no M_ROWNOTE_MAX in menu.c")
limit = int(m.group(1))

notes = 0
for fn in ("M_Display_Note", "M_Rendering_Note", "M_Graphics_Note"):
    start = menu.index(f"static const char *{fn} (int row)")
    body = menu[start:menu.index("\n}\n", start)]
    for row, text, cvar in re.findall(
            r"case\s+([A-Z_]+):\s*return\s+\"([^\"]*)\";\s*(?:/\*\s*([a-z_0-9]+)\s*\*/)?", body):
        notes += 1
        if len(text) > limit:
            errors.append(f"{fn}: {row} note is {len(text)} characters, over {limit}: \"{text}\"")
        if not cvar:
            continue	# a note about behaviour, not about one cvar (map-load notes)
        slider = re.search(r"\{\s*%s\s*,\s*\"%s\"" % (row, cvar), menu)
        handled = False
        for case in re.finditer(r"case\s+%s\s*:(.*?)(?=\n\tcase\s|\n\tdefault\s*:|\n}\n)" % row, menu, re.S):
            if re.search(r"Cvar_Set\w*\s*\(\s*\"%s\"" % cvar, case.group(1)):
                handled = True
        if not (slider or handled):
            errors.append(f"{fn}: {row}'s note names {cvar}, which that row does not set")

if notes < 10:
    errors.append(f"only {notes} notes parsed; the pattern no longer matches the source")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS {notes} row notes fit {limit} columns and name the cvar their row sets (#98)")
