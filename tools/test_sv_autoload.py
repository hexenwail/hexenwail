#!/usr/bin/env python3
"""sv_autoload: dying loads the last save made or loaded on this map, as in
Ironwail (GitHub #410).

Hexen II dies two ways (gamecode client.hc respawn): `restart restore`, or
with parm7 set a changelevel back to the same map, which PF_changelevel sends
as changelevel or changelevel2.  All three must try Host_AutoLoad first.  It
must check the save exists (our load Host_Errors rather than failing soft),
wait for an in-flight save, and honour Ironwail's 0/1/2/3 modes.  Every
successful save (autosave included) and every load records sv.lastsave.

Measured headless on demo1, sv_autosave 0, sv_autoload 3: save at 37 HP,
give health to 150, kill -> "Autoloading s5...", HUD back at 37.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []

host = read("engine/hexen2/host.c")
if 'cvar_t	sv_autoload = {"sv_autoload", "2", CVAR_ARCHIVE};' not in host:
    errors.append("sv_autoload must be registered with Ironwail's default 2, archived")
if "Cvar_RegisterVariable (&sv_autoload);" not in host:
    errors.append("sv_autoload must be registered")
if "lastsave[MAX_OSPATH]" not in read("engine/hexen2/server.h"):
    errors.append("server_t must carry lastsave")

cmd = read("engine/hexen2/host_cmd.c")
al = re.search(r"static qboolean Host_AutoLoad \(void\)\s*\{(.*?)\n\}", cmd, re.S)
if not al:
    errors.append("Host_AutoLoad missing")
else:
    b = al.group(1)
    if not re.search(r"^\tHost_WaitForSaveThread \(\);", b, re.M):
        errors.append("Host_AutoLoad must call Host_WaitForSaveThread")
    for need in ("!sv_autoload.value", "!sv.lastsave[0]", "svs.maxclients != 1", "cl.intermission",
                 "Sys_FileType (info) != FS_ENT_FILE",
                 "sv_autoload.value < 2.f", "SCR_ModalMessage", "sv_autoload.value < 3.f",
                 "v.health > 0.f", 'Cbuf_InsertText (va("load \\"%s\\"\\n", sv.lastsave));'):
        if need not in b:
            errors.append(f"Host_AutoLoad lacks {need!r}")
    if b.index("Host_WaitForSaveThread") > b.index("Sys_FileType"):
        errors.append("Host_AutoLoad must wait for the save thread before checking the save exists")

def body(name):
    m = re.search(r"static void %s \(void\)\s*\{(.*?)\n\}" % name, cmd, re.S)
    return m.group(1) if m else ""
r = body("Host_Restart_f")
if "if (Host_AutoLoad ())" not in r or r.index("Host_AutoLoad") > r.index("SV_SpawnServer"):
    errors.append("Host_Restart_f must try Host_AutoLoad before respawning")
for fn in ("Host_Changelevel_f", "Host_Changelevel2_f"):
    b = body(fn)
    if "!q_strcasecmp (sv.name, level) && Host_AutoLoad ()" not in b:
        errors.append(f"{fn} must try Host_AutoLoad on a same-map changelevel")
    elif b.index("Host_AutoLoad") > b.index("SV_SaveSpawnparms"):
        errors.append(f"{fn} must try Host_AutoLoad before saving spawn parms")

if not re.search(r"Host_SubmitSave\(&sd\);\s*\}\s*q_strlcpy \(sv\.lastsave, p, sizeof\(sv\.lastsave\)\);\s*/\*[^*]*\*/\s*return true;", cmd):
    errors.append("a successful save must record sv.lastsave")
lg = body("Host_Loadgame_f")
if "q_strlcpy (sv.lastsave, Cmd_Argv(1), sizeof(sv.lastsave));" not in lg or \
   lg.index("sv.lastsave") < lg.index("LoadGamestate"):
    errors.append("a load must record sv.lastsave after LoadGamestate spawned the map")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS sv_autoload loads the last save on death, Ironwail's modes (#410)")
