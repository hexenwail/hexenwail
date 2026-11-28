#!/usr/bin/env python3
"""The loading plaque stays live while a join is in progress (GitHub #571).

SCR_BeginLoadingPlaque used to draw one frame and then switch the screen off
until signon finished, so a remote join looked frozen for its whole length.
Now SCR_UpdateScreen redraws the plaque, throttled, with a status line set by
each stage of the Hexen II and HexenWorld joins.

This pins the parts that are easy to lose in a later edit:
- the throttle: a grace before the first live frame (a quick local load keeps
  its single plaque frame), at most 10 live frames a second, and at most 5% of
  the load's time spent drawing them, whatever a frame costs (llvmpipe, vsync);
- both renderers (gl_screen.c and the software screen.c, which WEBSOFT links)
  carry it, through the one shared include;
- a live frame never renders the world: it can land mid-precache;
- every stage names itself on the plaque, and the blocking loops tick it;
- HexenWorld no longer drops the plaque on a retry or a download;
- a local game (single player, a listen server's own client, a demo) shows
  no network stage: no "Connecting to local...", "Waiting for server info"
  or "Signon n/4";
- the 25 s "load timeout" counts realtime, not wall clock, so a slow local
  map load is not mistaken for a server that stopped answering;
- a connect retry updates the text but is not progress, so it never
  restarts that timeout.

A source pin: the live check is the headless connect run in the PR.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
errors = []


def read(path):
    try:
        return (ROOT / path).read_text()
    except FileNotFoundError:
        errors.append(f"{path}: missing")
        return ""


def body(src, signature, path):
    """The text of one C function, from its signature to the closing brace in
    column 0.  Missing is an error, not a skip."""
    m = re.search(re.escape(signature) + r"[^\n]*\n\{(.*?)\n\}", src, re.S)
    if not m:
        errors.append(f"{path}: {signature} not found")
        return ""
    return m.group(1)


def strip_comments(src):
    return re.sub(r"/\*.*?\*/", "", src, flags=re.S)


# --- the throttle -----------------------------------------------------------
inc_path = "engine/h2shared/scr_loading.inc"
inc = read(inc_path)
grace = re.search(r"#define\s+SCR_LOADING_GRACE\s+([0-9.]+)", inc)
interval = re.search(r"#define\s+SCR_LOADING_INTERVAL\s+([0-9.]+)", inc)
share = re.search(r"#define\s+SCR_LOADING_MAXSHARE\s+([0-9.]+)", inc)
if not grace or float(grace.group(1)) < 0.25:
    errors.append(f"{inc_path}: SCR_LOADING_GRACE must exist and be >= 0.25 s, "
                  "or quick local loads start drawing extra frames")
if not interval or float(interval.group(1)) < 0.1:
    errors.append(f"{inc_path}: SCR_LOADING_INTERVAL must exist and be >= 0.1 s "
                  "(at most 10 live frames a second)")
if not share or not 0 < float(share.group(1)) <= 0.1:
    errors.append(f"{inc_path}: SCR_LOADING_MAXSHARE must exist and be at most 0.1: "
                  "live frames may not take more than a tenth of a load")
due = body(inc, "static qboolean SCR_LoadingFrameDue (void)", inc_path)
if "SCR_LOADING_GRACE" not in due or "scr_loadnextdraw" not in due:
    errors.append(f"{inc_path}: SCR_LoadingFrameDue must test both the grace and the next due time")
done = body(inc, "static void SCR_LoadingFrameDone (void)", inc_path)
if "SCR_LOADING_MAXSHARE" not in done or "SCR_LOADING_INTERVAL" not in done:
    errors.append(f"{inc_path}: SCR_LoadingFrameDone must space frames by their cost "
                  "(MAXSHARE) and by the 10/s cap (INTERVAL)")
if "Sys_DoubleTime" not in due:
    errors.append(f"{inc_path}: the throttle must run on Sys_DoubleTime: realtime "
                  "stands still inside a blocking precache loop")
tick = body(inc, "void SCR_LoadingTick (void)", inc_path)
if "scr_disabled_for_loading" not in tick:
    errors.append(f"{inc_path}: SCR_LoadingTick must do nothing unless the plaque is up")
settext = body(inc, "static void SCR_LoadingSetText (qboolean progress, const char *fmt, va_list argptr)", inc_path)
if not re.search(r"if \(progress\)\s*scr_disabled_time = realtime;", settext):
    errors.append(f"{inc_path}: progress, and only progress, must restart the load "
                  "timeout, on realtime (see the timeout check below)")
status = body(inc, "void SCR_SetLoadingStatus (const char *fmt, ...)", inc_path)
if "SCR_LoadingSetText (true, fmt, argptr);" not in status:
    errors.append(f"{inc_path}: SCR_SetLoadingStatus is progress: it must restart the timeout")
retry = body(inc, "void SCR_SetLoadingRetry (const char *fmt, ...)", inc_path)
if "SCR_LoadingSetText (false, fmt, argptr);" not in retry:
    errors.append(f"{inc_path}: SCR_SetLoadingRetry must update the text without "
                  "restarting the timeout")
if "scr_loadingintick" not in tick:
    errors.append(f"{inc_path}: SCR_LoadingTick must mark itself, so the timeout "
                  "never drops the plaque mid-precache")
gap = re.search(r"#define\s+SCR_LOADING_MAXGAP\s+([0-9.]+)", inc)
if not gap or float(gap.group(1)) > 2.0:
    errors.append(f"{inc_path}: SCR_LOADING_MAXGAP must cap the gap at 2 s or less, "
                  "or one hitch freezes the clock")
draw = body(inc, "static void SCR_DrawLoadingStatus (int y)", inc_path)
if "scr_loadinglive" not in draw:
    errors.append(f"{inc_path}: the status line must draw on live frames only, "
                  "so the plaque's first frame stays as it was")
# 2D seams only: this file is shared with the 8bpp renderer and -gpu.
if re.search(r"\bgl[A-Z]\w*\s*\(|_fp\s*\(", strip_comments(inc)):
    errors.append(f"{inc_path}: no raw GL; draw through the Draw_* seams")

# --- both renderers ---------------------------------------------------------
for path in ("engine/h2shared/gl_screen.c", "engine/h2shared/screen.c"):
    src = read(path)
    if '#include "scr_loading.inc"' not in src:
        errors.append(f"{path}: must include scr_loading.inc")
    upd = body(src, "void SCR_UpdateScreen (void)", path)
    if "SCR_LoadingFrameDue ()" not in upd:
        errors.append(f"{path}: SCR_UpdateScreen must draw a live frame when one is due")
    if "SCR_LoadingFrameDone ();" not in src:
        errors.append(f"{path}: a live frame must report its cost (SCR_LoadingFrameDone)")
    begin = body(src, "void SCR_BeginLoadingPlaque (void)", path)
    if "SCR_LoadingBegin ();" not in begin:
        errors.append(f"{path}: SCR_BeginLoadingPlaque must start the status clock")
    end = body(src, "void SCR_EndLoadingPlaque (void)", path)
    if "SCR_LoadingEnd ();" not in end:
        errors.append(f"{path}: SCR_EndLoadingPlaque must clear the status line")
    # The timeout is for a server that stopped answering, so it counts only
    # time the host loop ran free: realtime, which Sys_MainFrame advances by
    # 0.1 s for any frame over 0.5 s.  On Sys_DoubleTime the engine's own
    # blocking load counted, and a slow single-player `map` fired "load
    # timeout." at Host_Connect_f's plaque, right after SV_SpawnServer.
    if not re.search(r"if \(!scr_loadingintick && realtime - scr_disabled_time > 25\)", upd):
        errors.append(f"{path}: the load timeout must run on realtime, not wall clock, "
                      "and never inside SCR_LoadingTick")
    if "scr_disabled_time = realtime;" not in begin:
        errors.append(f"{path}: SCR_BeginLoadingPlaque must start the load timeout on realtime")
    if re.search(r"Sys_DoubleTime \(\)\s*-\s*scr_disabled_time|scr_disabled_time\s*=\s*Sys_DoubleTime", src):
        errors.append(f"{path}: the load timeout must not use Sys_DoubleTime: a long local "
                      "load is not a stalled server")
    dl = body(src, "void SCR_DrawLoading (void)", path)
    if "SCR_DrawLoadingStatus (" not in dl:
        errors.append(f"{path}: SCR_DrawLoading must draw the status line")

gl = read("engine/h2shared/gl_screen.c")
upd = body(gl, "void SCR_UpdateScreen (void)", "gl_screen.c")
if not re.search(r"if \(!scr_loadinglive\)\s*\n(?:#[^\n]*\n|[^\n]*\n){0,6}?\s*V_RenderView \(\);", upd):
    errors.append("gl_screen.c: a live loading frame must skip V_RenderView "
                  "(it can land mid-precache, the #204 class)")
if "else if (cl.intermission && !scr_loadinglive)" not in upd:
    errors.append("gl_screen.c: a live frame must take the plaque branch, not the "
                  "intermission overlay over a back buffer it never drew")
if "scr_drawloading = was_drawloading;" not in upd:
    errors.append("gl_screen.c: SCR_UpdateScreen must restore scr_drawloading after a live frame")

# --- every stage names itself -----------------------------------------------
# (file, function signature, status text, needs a tick after it)
STAGES = [
    ("engine/hexen2/host_cmd.c", "static void Host_Connect_f (void)", "Connecting to %s...", False),
    ("engine/hexen2/cl_main.c", "void CL_EstablishConnection (const char *host)", "Waiting for server info", False),
    ("engine/hexen2/cl_main.c", "void CL_SignonReply (void)", "Signon 1/4", False),
    ("engine/hexen2/cl_main.c", "void CL_SignonReply (void)", "Signon 2/4", False),
    ("engine/hexen2/cl_main.c", "void CL_SignonReply (void)", "Signon 3/4", False),
    ("engine/hexen2/cl_parse.c", "static void CL_ParseServerInfo (void)", "Loading map %s", True),
    ("engine/hexen2/cl_parse.c", "static void CL_ParseServerInfo (void)", "Precaching models %d/%d", True),
    ("engine/hexen2/cl_parse.c", "static void CL_ParseServerInfo (void)", "Precaching sounds %d/%d", True),
    ("engine/hexen2/cl_parse.c", "static void CL_ParseServerInfo (void)", "Building world", True),
    ("engine/hexen2/cl_hw.c", "static void HWCL_ConnectionlessPacket (void)", "Waiting for server info", False),
    ("engine/hexen2/cl_hw.c", "static void HWCL_ParseServerData (void)", "Receiving sound list", False),
    ("engine/hexen2/cl_hw.c", "static qboolean HWCL_LoadModels (void)", "Loading map %s", True),
    ("engine/hexen2/cl_hw.c", "static qboolean HWCL_LoadModels (void)", "Precaching models %d/%d", True),
    ("engine/hexen2/cl_hw.c", "static qboolean HWCL_LoadModels (void)", "Building world", True),
    ("engine/hexen2/cl_hw.c", "static void HWCL_LoadSounds (void)", "Precaching sounds %d/%d", True),
    ("engine/hexen2/cl_hw_download.inc", "static void HWCL_PrintDownloadProgress (int percent, const char *status)", "Downloading %d%% (%ld KB) %s", False),
    ("engine/hexen2/cl_hw_download.inc", "static qboolean HWCL_CheckOrDownloadFile (const char *filename)", "Downloading %s", False),
    ("engine/hexen2/cl_hw_download.inc", "static void HWCL_SoundNextDownload (void)", "Receiving model list", False),
    ("engine/hexen2/cl_hw_download.inc", "static void HWCL_ModelNextDownload (void)", "Receiving world state", False),
    ("engine/hexen2/cl_hw_download.inc", "static void HWCL_SkinNextDownload (void)", "Checking player skins", False),
]
for path, sig, text, ticks in STAGES:
    fn = body(read(path), sig, path)
    m = re.search(r'SCR_SetLoadingStatus \((?:local \? "[^"]*" : )?"[^"]*' + re.escape(text), fn)
    if not m:
        errors.append(f"{path}: {sig.split('(')[0].split()[-1].lstrip('*')} must set the status '{text}'")
    elif ticks and "SCR_LoadingTick ();" not in fn[m.end():m.end() + 300]:
        errors.append(f"{path}: '{text}' is a blocking step and must be followed by SCR_LoadingTick ()")

# A connect retry is the client resending, not the server answering.  It
# names itself on the plaque but must not restart the 25 s load timeout:
# HexenWorld resent every 5 s and each "(retry N)" reset the clock, so a
# dead server's plaque could only end through the separate 15 s handshake
# timeout, and the load timeout itself never fired.
RETRY_SITES = [
    ("engine/hexen2/net_dgrm.c", "static qsocket_t *_Datagram_Connect (const char *host)"),
    ("engine/hexen2/cl_hw.c", "static void HWCL_SendConnectPacket (void)"),
]
for path, sig in RETRY_SITES:
    fn = body(read(path), sig, path)
    if not re.search(r'SCR_SetLoadingRetry \("[^"]*\(retry %d\)', fn):
        errors.append(f"{path}: {sig.split('(')[0].split()[-1].lstrip('*')} must show its "
                      "retries through SCR_SetLoadingRetry")
for path in ("engine/hexen2/net_dgrm.c", "engine/hexen2/cl_hw.c", "engine/hexen2/cl_main.c",
             "engine/hexen2/host_cmd.c", "engine/hexen2/cl_parse.c", "engine/hexen2/cl_hw_download.inc"):
    if re.search(r'SCR_SetLoadingStatus \([^;]*retry', strip_comments(read(path)), re.I):
        errors.append(f"{path}: a retry must go through SCR_SetLoadingRetry, not "
                      "SCR_SetLoadingStatus (it is not progress)")

# The datagram handshake blocks for up to 7.5 s; its wait loop ticks.
dg = body(read("engine/hexen2/net_dgrm.c"),
          "static qsocket_t *_Datagram_Connect (const char *host)", "net_dgrm.c")
if not re.search(r"do\s*\{\s*SCR_LoadingTick \(\);", dg):
    errors.append("net_dgrm.c: _Datagram_Connect's reply wait must tick the plaque")

# The plaque goes up before the handshake, not after it.
conn = body(read("engine/hexen2/host_cmd.c"), "static void Host_Connect_f (void)", "host_cmd.c")
b, e = conn.find("SCR_BeginLoadingPlaque ();"), conn.find("CL_EstablishConnection (name);")
if b < 0 or e < 0 or b > e:
    errors.append("host_cmd.c: Host_Connect_f must raise the plaque before CL_EstablishConnection")

# Leaving a HexenWorld server ends the plaque (HWCL_Disconnect), so the old
# connection goes before the plaque comes up.
d = conn.find("CL_Disconnect ();", conn.find("q_strlcpy (name"))
if d < 0 or d > b:
    errors.append("host_cmd.c: Host_Connect_f must disconnect before raising the plaque")

# Esc drops the plaque in the key handler, not in M_ToggleMenu_f, which
# CL_StopPlayback calls mid-load.
keys = read("engine/hexen2/keys.c")
if not re.search(r"if \(key_dest == key_game && scr_disabled_for_loading\)\s*SCR_EndLoadingPlaque \(\);\s*M_ToggleMenu_f \(\);", keys):
    errors.append("keys.c: Esc over the plaque must drop it before opening the menu")
if "SCR_EndLoadingPlaque" in strip_comments(body(read("engine/hexen2/menu.c"), "void M_ToggleMenu_f (void)", "menu.c")):
    errors.append("menu.c: M_ToggleMenu_f must not drop the plaque (CL_StopPlayback calls it mid-load)")

# HexenWorld used to drop the plaque to show console text instead.
hw = read("engine/hexen2/cl_hw.c")
frame = strip_comments(body(hw, "void HWCL_Frame (void)", "cl_hw.c"))
if "SCR_EndLoadingPlaque" in frame:
    errors.append("cl_hw.c: HWCL_Frame must not drop the plaque on a connect retry")
chk = strip_comments(body(read("engine/hexen2/cl_hw_download.inc"),
                          "static qboolean HWCL_CheckOrDownloadFile (const char *filename)",
                          "cl_hw_download.inc"))
if "SCR_EndLoadingPlaque" in chk:
    errors.append("cl_hw_download.inc: a download must not drop the plaque")

# --- single player says nothing about networks -----------------------------
# A local game (single player, a listen server's own client, a demo) signs on
# over loopback through the same stages, so without these guards a slow local
# map load flashed "Connecting to local...", "Waiting for server info" and
# "Signon n/4" on the plaque.  Remote Hexen II joins keep every stage above.
NETWORDS = re.compile(r"connect|retry|signon|server|waiting|timeout", re.I)
conn_src = strip_comments(conn)
if not re.search(r'if \(q_strcasecmp \(name, "local"\)\)\s*SCR_SetLoadingStatus \("Connecting to %s\.\.\.", name\);',
                 conn_src):
    errors.append('host_cmd.c: Host_Connect_f must not say "Connecting to local..." '
                  "for a local game")
est = strip_comments(body(read("engine/hexen2/cl_main.c"),
                          "void CL_EstablishConnection (const char *host)", "cl_main.c"))
if not re.search(r'if \(!NET_IsLocalConnection \(cls\.netcon\)\)\s*SCR_SetLoadingStatus \("Waiting for server info"\);', est):
    errors.append('cl_main.c: CL_EstablishConnection must not say "Waiting for server info" '
                  "over loopback")
elif est.find("cls.netcon = NET_Connect (host);") > est.find("NET_IsLocalConnection (cls.netcon)"):
    errors.append("cl_main.c: CL_EstablishConnection must test NET_IsLocalConnection after "
                  "NET_Connect, or cls.netcon is still NULL and every game reads as remote")
sig = strip_comments(body(read("engine/hexen2/cl_main.c"), "void CL_SignonReply (void)", "cl_main.c"))
if not re.search(r"qboolean\s+local = cls\.demoplayback \|\| NET_IsLocalConnection \(cls\.netcon\);", sig):
    errors.append("cl_main.c: CL_SignonReply must know a local (loopback or demo) signon")
calls = re.findall(r"SCR_SetLoadingStatus \(([^;]*)\);", sig)
if len(calls) != 3:
    errors.append(f"cl_main.c: CL_SignonReply should set 3 stages, found {len(calls)}")
for c in calls:
    m = re.fullmatch(r'local \? "([^"]*)" : "(Signon [1-3]/4[^"]*)"', c.strip())
    if not m:
        errors.append(f"cl_main.c: CL_SignonReply stage {c.strip()} must pick a neutral "
                      "local text before its \"Signon n/4\" one")
    elif NETWORDS.search(m.group(1)):
        errors.append(f"cl_main.c: local signon text \"{m.group(1)}\" names the network")
# What a local `map` shows before the client takes over.
hmap = strip_comments(body(read("engine/hexen2/host_cmd.c"), "static void Host_Map_f (void)", "host_cmd.c"))
if 'SCR_SetLoadingStatus ("Loading map %s", Cmd_Argv(1));' not in hmap:
    errors.append('host_cmd.c: Host_Map_f must say "Loading map %s" (the local game\'s stage '
                  "while the server spawns)")

# The realtime timeout above depends on this clamp: a frame that took more
# than half a second (a blocking load) adds only 0.1 s to realtime.
mf = strip_comments(body(read("engine/h2shared/sys_sdl.c"), "static void Sys_MainFrame (void)", "sys_sdl.c"))
if not re.search(r"if \(time > 0\.5\)\s*time = 0\.1;", mf):
    errors.append("sys_sdl.c: Sys_MainFrame must fold a frame over 0.5 s into 0.1 s, "
                  "or the realtime load timeout counts blocking loads again")

# Every caller sees a prototype.
hdr = read("engine/h2shared/screen.h")
for proto in ("void SCR_SetLoadingStatus (const char *fmt, ...) FUNC_PRINTF(1,2);",
              "void SCR_SetLoadingRetry (const char *fmt, ...) FUNC_PRINTF(1,2);",
              "void SCR_LoadingTick (void);"):
    if proto not in hdr:
        errors.append(f"screen.h: missing '{proto}'")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print(f"PASS the loading plaque stays live, every join stage names itself "
      f"({len(STAGES)} stages, #571), and a local game shows no network stage "
      f"or wall-clock load timeout")
