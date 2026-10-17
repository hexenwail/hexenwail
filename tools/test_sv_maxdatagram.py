#!/usr/bin/env python3
"""sv_maxdatagram caps the per-frame unreliable datagram (GitHub #82).

Default 0 = no cap beyond connection and protocol, so nothing changes unless
set.  When set it applies to every connection, local included (it is also
the test lever for sv_netsort's shedding, which stock maps never reach),
after the protocol clamp, floored at 1024 so the svc_clear_edicts reserve
leaves room.  Measured: demo1 + 600 item_health in view, first full update
3678 B / 621 entities uncapped, 660 B / 108 at 1400, 336 B / 54 at 1100,
the rest arriving over the next frames; reliable signon untouched.
"""
from pathlib import Path
import re
import sys

src = (Path(__file__).resolve().parents[1] / "engine/hexen2/sv_main.c").read_text()
errors = []
if 'cvar_t	sv_maxdatagram		= {"sv_maxdatagram", "0", CVAR_NONE};' not in src or \
   "Cvar_RegisterVariable (&sv_maxdatagram);" not in src:
    errors.append("sv_maxdatagram must be registered, default 0 (no cap)")
if "#define	SV_MAXDATAGRAM_FLOOR	MAX_DATAGRAM_OLD" not in src:
    errors.append("the cap must be floored at MAX_DATAGRAM_OLD (1024)")
fn = re.search(r"static qboolean SV_SendClientDatagram \(client_t \*client\)\s*\{(.*?)\n\}", src, re.S)
body = fn.group(1) if fn else ""
cap = body.find("if (sv_maxdatagram.integer > 0)")
if cap < 0:
    errors.append("SV_SendClientDatagram must apply sv_maxdatagram")
else:
    if cap < body.find("SV_MaxDatagram ()") or cap > body.find("SZ_Init (&msg, buf, wire);"):
        errors.append("the cap must come after the protocol clamp and before the buffer is sized")
    if "NET_IsLocalConnection" in body[cap:body.find("SZ_Init (&msg, buf, wire);")]:
        errors.append("the cap must apply to local connections too (test lever)")
    if "if (cap < SV_MAXDATAGRAM_FLOOR)" not in body[cap:]:
        errors.append("the cap must honour SV_MAXDATAGRAM_FLOOR")
if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS sv_maxdatagram caps the per-frame datagram, default off (#82)")
