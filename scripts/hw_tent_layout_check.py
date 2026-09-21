#!/usr/bin/env python3
"""Check the HexenWorld temp-entity reader against the gamecode writers.

engine/hexen2/cl_hw_tent.inc has to consume every svc_temp_entity payload
byte for byte, or the rest of the packet -- reliable data included -- is
misread (GitHub #213).  Its fixed sizes were transcribed from the original
client's reader; this script derives them independently from what the
server actually writes, by walking gamecode/hc/hw/*.hc:

    WriteByte (MSG_x, SVC_TEMPENTITY);
    WriteByte (MSG_x, TE_FOO);
    Write<Coord|Short|Entity|Byte|Char|Angle|Long|Float|String> ...
    multicast (...)            <- or the next SVC_* / end of function

and summing wire sizes (coord/short/entity 2, byte/char/angle 1, long/float
4).  Every writer of a type must agree with every other and with the table.
Types whose size depends on their contents (loops in the writer) are listed
in VARIABLE and checked by scripts/hw_tent_test.c instead.

Exit 0 when everything agrees, 1 on any mismatch.  No arguments; run from
anywhere.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HC_DIR = os.path.join(ROOT, "gamecode", "hc", "hw")
INC = os.path.join(ROOT, "engine", "hexen2", "cl_hw_tent.inc")

SIZES = {"Coord": 2, "Short": 2, "Entity": 2, "Byte": 1, "Char": 1,
         "Angle": 1, "Long": 4, "Float": 4}

# Writers with a loop or a count-dependent tail: sizes are data-dependent.
VARIABLE = {"TE_SUNSTAFF_CHEAP", "TE_CHAINLIGHTNING"}

# Handed to the Hexen II renderer, which reads the same layout; checked here
# against the layouts CL_ParseTEntType reads (cl_tent.c).
HEXEN2 = {"TE_SPIKE": 6, "TE_SUPERSPIKE": 6, "TE_EXPLOSION": 6,
          "TE_WIZSPIKE": 6, "TE_KNIGHTSPIKE": 6, "TE_LAVASPLASH": 6,
          "TE_TELEPORT": 6, "TE_LIGHTNING1": 14, "TE_LIGHTNING2": 14,
          "TE_LIGHTNING3": 14}
STREAMS = {"TE_STREAM_CHAIN", "TE_STREAM_SUNSTAFF1", "TE_STREAM_SUNSTAFF2",
           "TE_STREAM_LIGHTNING", "TE_STREAM_LIGHTNING_SMALL",
           "TE_STREAM_COLORBEAM", "TE_STREAM_ICECHUNKS", "TE_STREAM_GAZE",
           "TE_STREAM_FAMINE"}

WRITE_RE = re.compile(r"\bWrite(Coord|Short|Entity|Byte|Char|Angle|Long|Float|String)\s*\(\s*MSG_\w+\s*,\s*([^;]*?)\)\s*;")
END_RE = re.compile(r"\bmulticast\s*\(|^\s*\}|\breturn\b|\bfor\s*\(|\bwhile\s*\(|\bif\s*\(")


def strip_comments(text):
    # Keep the newlines a block comment spans, so reported sites keep the
    # file's own line numbers.
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def writer_sizes():
    """{TE_NAME: set of payload sizes}, {TE_NAME: [site, ...]} for sites
    whose writer is cut short by control flow before a multicast."""
    found, unsure = {}, {}
    for name in sorted(os.listdir(HC_DIR)):
        if not name.endswith(".hc"):
            continue
        path = os.path.join(HC_DIR, name)
        lines = strip_comments(open(path, encoding="latin-1").read()).split("\n")
        for i, line in enumerate(lines):
            if not re.search(r"Write\w+\s*\([^;]*SVC_TEMPENTITY\s*\)", line):
                continue
            # the type byte is the next Write on this or following lines
            te, size, j, clean = None, 0, i, True
            rest = line[line.index("SVC_TEMPENTITY") + len("SVC_TEMPENTITY"):]
            chunk = [rest] + lines[i + 1:]
            for k, text in enumerate(chunk):
                pieces = WRITE_RE.findall(text)
                for kind, arg in pieces:
                    if te is None:
                        te = arg.strip()
                        continue
                    if kind == "String":
                        clean = False
                        continue
                    size += SIZES[kind]
                if te is not None and k > 0 and END_RE.search(text) and \
                        not WRITE_RE.search(text):
                    if not re.search(r"\bmulticast\s*\(", text):
                        clean = False
                    break
                if k > 60:
                    clean = False
                    break
            if te is None:
                continue
            site = "%s:%d" % (name, i + 1)
            if clean:
                found.setdefault(te, set()).add(size)
                found.setdefault(te + "@", []).append((site, size))
            else:
                unsure.setdefault(te, []).append(site)
    return found, unsure


def reader_sizes():
    text = open(INC, encoding="latin-1").read()
    table = {}
    for m in re.finditer(r"\[HWTE_(\w+)\]\s*=\s*([0-9+ ]+),", text):
        table["TE_" + m.group(1)] = sum(int(x) for x in m.group(2).split("+"))
    # the four drawn in the switch, not the table
    for name, size in (("TE_GUNSHOT", 7), ("TE_BLOOD", 7),
                       ("TE_LIGHTNINGBLOOD", 6), ("TE_TAREXPLOSION", 6)):
        table.setdefault(name, size)
    return table


def main():
    found, unsure = writer_sizes()
    table = reader_sizes()
    bad = 0
    types = sorted(k for k in found if not k.endswith("@"))
    for te in types:
        sizes = found[te]
        if te in VARIABLE:
            continue
        if te in STREAMS:
            ok = all(s in (16, 17) for s in sizes)
            want = "16 (17 for COLORBEAM)"
        elif te in HEXEN2:
            want = HEXEN2[te]
            ok = sizes == {want}
        else:
            want = table.get(te)
            ok = sizes == {want}
        if not ok:
            bad += 1
            sites = ", ".join("%s=%d" % s for s in found[te + "@"])
            print("MISMATCH %-26s reader %s, writers %s" % (te, want, sites))
    for te, sites in sorted(unsure.items()):
        if te in VARIABLE or te in found:
            continue
        print("note: %s written only under control flow (%s); not size-checked"
              % (te, ", ".join(sites)))
    missing = [te for te in types if te not in VARIABLE and te not in STREAMS
               and te not in HEXEN2 and te not in table]
    for te in missing:
        bad += 1
        print("MISSING  %-26s written by gamecode, no reader size" % te)
    print("%d temp entity types written by gamecode, %d checked, %d problem(s)"
          % (len(types), len(types) - len(VARIABLE & set(types)), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
