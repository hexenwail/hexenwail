#!/usr/bin/env python3
"""cl_startmenu: Ironwail's start-at-the-menu toggle (GitHub #273).

Upstream (08d57813) gates the menu_main its Host_Startdemos_f inserts.
Hexen II's hexen.rc also runs `menu_main` itself after autoexec.cfg, so the
port must gate that call too, while the startup script runs -- or
cl_startmenu 0 would do nothing on retail data.  Verified headless: retail 1
-> menu, 0 -> console; SoT (startdemos "menu.dem") 1 -> menu over the demo,
0 -> the demo alone.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []
if 'cvar_t	cl_startmenu = {"cl_startmenu", "1", CVAR_ARCHIVE};' not in read("engine/hexen2/cl_main.c") or \
   "Cvar_RegisterVariable (&cl_startmenu);" not in read("engine/hexen2/cl_main.c"):
    errors.append("cl_startmenu must be registered, default 1, archived, as upstream")
sd = re.search(r"static void Host_Startdemos_f \(void\)\s*\{(.*?)\n\}", read("engine/hexen2/host_cmd.c"), re.S)
if not sd or not re.search(r"if \(cl_startmenu\.integer\)\s*Cbuf_InsertText \(\"menu_main\\n\"\);", sd.group(1)):
    errors.append("Host_Startdemos_f must insert menu_main only when cl_startmenu is set")
elif sd.group(1).index("cl_startmenu") > sd.group(1).index("cl_startdemos"):
    errors.append("the menu must be queued before the cl_startdemos check, as upstream")
menu = read("engine/hexen2/menu.c")
if 'Cmd_AddCommand ("menu_main", M_Menu_Main_Cmd_f);' not in menu or \
   "if (host_startup_script && !cl_startmenu.integer)\n\t\treturn;" not in menu:
    errors.append("the menu_main command must stand down during the startup script when cl_startmenu is 0")
host = read("engine/hexen2/host.c")
if not re.search(r"host_startup_script = true;\s*if \(!setjmp\(host_abort\)\)[^\n]*\n\s*Cbuf_Execute \(\);\s*host_startup_script = false;", host):
    errors.append("host_startup_script must bracket the startup script's execution, error path included")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS cl_startmenu gates hexen.rc's menu and startdemos' (#273)")
