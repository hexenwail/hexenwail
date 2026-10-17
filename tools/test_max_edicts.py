#!/usr/bin/env python3
"""max_edicts reaches Ironwail's 16384, and what goes on the wire past 8192
is either encoded whole or not sent (GitHub #411).

MAX_EDICTS was 8192, so the `max_edicts` cvar could only lower the limit.
It is now Ironwail's ceiling (32000) and default (16384).  Past 8192 an
entity no longer fits the packed (ent<<3)|channel short every Raven-lineage
sound message uses, so protocol 100 sends it whole (SND_LARGEENTITY,
svc_stopsound_large, svc_sound_update_pos_large) and 18..21 drop the sound.

Also held here, both found on the way:
  - EDICT_NUM bounds by sv_max_edicts (what sv.edicts holds), not MAX_EDICTS;
  - the client reads the packed short UNSIGNED -- signed, every entity from
    4096 up decoded negative.

Measured: a demo1 with 9000 extra persistent entities runs at 9303 edicts on
h2ded where master stops with "ED_Alloc: no free edicts"; on protocol 100 the
client receives start/stop sounds for entity 9284.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
read = lambda p: (ROOT / p).read_text()
errors = []

q = read("engine/hexen2/quakedef.h")
m = re.search(r"#define\s+MAX_EDICTS\s+(\d+)", q)
if not m or int(m.group(1)) < 16384:
    errors.append("MAX_EDICTS must be at least Ironwail's 16384 default")
if not re.search(r'cvar_t\s+max_edicts\s*=\s*\{"max_edicts", "16384", CVAR_NONE\};', read("engine/hexen2/sv_main.c")):
    errors.append("max_edicts must default to 16384, unarchived, as Ironwail")

srv = read("engine/hexen2/sv_main.c")
fits = re.search(r"static qboolean SV_SoundEntityFits \(int ent, qboolean \*large\)\s*\{(.*?)\n\}", srv, re.S)
if not fits or "ent >= SND_MAX_PACKED_ENTITY" not in fits.group(1) or "sv_protocol >= PROTOCOL_HEXENWAIL_1" not in fits.group(1):
    errors.append("SV_SoundEntityFits must allow large entities on protocol 100 only")
for fn in ("SV_StartSound", "SV_StopSound", "SV_UpdateSoundPos"):
    body = re.search(r"void %s \([^)]*\)\s*\{(.*?)\n\}" % fn, srv, re.S)
    if not body or "if (!SV_SoundEntityFits (ent, &large))" not in body.group(1):
        errors.append(f"{fn} must check SV_SoundEntityFits before writing")
    elif "channel = (ent<<3) | channel;" in body.group(1).split("SV_SoundEntityFits")[0]:
        errors.append(f"{fn} packs the entity before checking it fits")

p = read("engine/hexen2/protocol.h")
for need in ("#define	SND_LARGEENTITY	(1<<4)", "#define	SND_MAX_PACKED_ENTITY	8192",
             "#define	svc_stopsound_large		57", "#define	svc_sound_update_pos_large	58"):
    if need not in p:
        errors.append(f"protocol.h lacks {need!r}")
if "SND_LARGEENTITY	(1<<3)" in p:
    errors.append("bit 3 is SND_OVERFLOW2 here; SND_LARGEENTITY must not reuse it")

cl = read("engine/hexen2/cl_parse.c")
if cl.count("(unsigned short) MSG_ReadShort ()") + cl.count("(unsigned short) MSG_ReadShort()") < 5:
    errors.append("every sound entity short must be read unsigned")
if re.search(r"ent(_num)? > MAX_EDICTS", cl):
    errors.append("client sound bounds must be >= MAX_EDICTS, not >")
for svc in ("case svc_stopsound_large:", "case svc_sound_update_pos_large:", '"svc_sound_update_pos_large"'):
    if svc not in cl:
        errors.append(f"cl_parse.c lacks {svc}")

ed = re.search(r"edict_t \*EDICT_NUM\(int n\)\s*\{(.*?)\n\}", read("engine/h2shared/pr_edict.c"), re.S)
if not ed or "n >= sv_max_edicts" not in ed.group(1):
    errors.append("EDICT_NUM must bound by sv_max_edicts")

sig = re.search(r"#define\s+MAX_SIGNON_BUFFERS\s+(\d+)", read("engine/hexen2/server.h"))
sigsize = 32768 // 2
if not sig or m and int(sig.group(1)) * sigsize < int(m.group(1)) * 20:
    errors.append("MAX_SIGNON_BUFFERS must hold a 20-byte baseline for every edict")

if errors:
    sys.exit("FAIL:\n  " + "\n  ".join(errors))
print("PASS max_edicts 16384 of 32000; sounds past 8192 whole on 100, dropped elsewhere (#411)")
