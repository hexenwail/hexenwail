#!/usr/bin/env python3
"""Picking a map in the Maps browser goes through Ironwail's skill step,
which offers "Resume last game" when the map has an autosave (GitHub #410).

That step is the only way an autosave is reachable from the menus: the Load
menu enumerates numbered slots only.  Held here: Enter in the browser opens
the step (and does NOT hand the keyboard back to the game first -- the bug
the first cut had, which closed the menu and loaded nothing); the step finds
autosave/<map>/info.dat; Resume is preselected when present; Enter loads
"autosave/<map>" or sets skill and starts the map; Escape returns to the
browser; the state is wired into draw and key dispatch.

Measured headless on demo1: autosave at 41 HP, health raised to 99, F9
(menu_maps) -> "demo1" -> Enter shows the class's four skills and "Resume
last game / from 29 seconds ago" -> Enter loads autosave/demo1, HUD at 41.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
m = (ROOT / "engine/hexen2/menu.c").read_text()
errors = []

if "m_mapskill," not in (ROOT / "engine/hexen2/menu.h").read_text():
    errors.append("menu.h lacks m_mapskill")

mk = re.search(r"static void M_Maps_Key \(int key\)\s*\{(.*?)\n\}", m, re.S)
enter = re.search(r"case K_ENTER:\s*case K_GP_A:(.*?)(?:\n\tcase |\Z)", mk.group(1), re.S) if mk else None
if not enter or "M_Maps_OpenSkill (&maps_list[maps_view[maps_cursor]]);" not in enter.group(1):
    errors.append("Enter in the Maps browser must open the skill step")
elif "Key_SetDest (key_game)" in enter.group(1) or "m_state = m_none" in enter.group(1):
    errors.append("Enter in the Maps browser must not leave the menu before the skill step")

op = re.search(r"static void M_Maps_OpenSkill \(const mapentry_t \*e\)\s*\{(.*?)\n\}", m, re.S)
if not op or 'va("autosave/%s/info.dat", e->name)' not in op.group(1) or \
   "mapskill_cursor = MAPSKILL_RESUME;" not in op.group(1) or "m_state = m_mapskill;" not in op.group(1):
    errors.append("M_Maps_OpenSkill must look for the autosave, preselect Resume, and enter m_mapskill")

k = re.search(r"static void M_MapSkill_Key \(int key\)\s*\{(.*?)\n\}", m, re.S)
if not k:
    errors.append("M_MapSkill_Key missing")
else:
    b = k.group(1)
    for need in ('load \\"autosave/%s\\"\\n', 'va("skill %d\\n", mapskill_cursor)', 'va("map %s\\n", mapskill_name)',
                 "m_state = m_maps;", "Key_SetDest (key_game);"):
        if need not in b:
            errors.append(f"M_MapSkill_Key lacks {need!r}")

if not re.search(r"case m_mapskill:\s*M_MapSkill_Draw \(\);", m):
    errors.append("m_mapskill is not drawn")
if not re.search(r"case m_mapskill:\s*M_MapSkill_Key \(key\);", m):
    errors.append("m_mapskill gets no keys")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS the Maps browser's skill step resumes a map's autosave (#410)")
