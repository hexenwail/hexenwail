#!/usr/bin/env python3
"""
Print the two CRCs PR_ClassifyGamecode() identifies a progs.dat by.

WHY THIS EXISTS (uhexen2-qvqk)

PR_ClassifyGamecode() (engine/h2shared/pr_edict.c) names Raven's gamecode from
a table of whole-file CRCs measured off a retail install:

    static const unsigned int retail_crcs[] = { 17499U, 20799U, 33075U };

Those three are the only retail images anyone here has had in hand, and they
are 1.11/1.12a.  A player on an older Raven release -- 1.03, or the 1.09 whose
progdefs CRC is aliased to 1.11's -- has a whole-file CRC that is not in that
table and no Hexenwail marker, so the startup line and the Game Options footer
call their retail gamecode "Third-party".  Closing that hole needs nothing but
the missing numbers, and the numbers need nothing but the file.

Reading them off the engine's own "Gamecode:" line means owning that release
and running it.  This reads them straight out of the file instead, so anyone
who can put a 1.03 or 1.09 progs.dat in front of a shell can report the value
without installing anything.

WHAT THE TWO NUMBERS ARE

  file crc      CRC over the entire file, engine-side `pr_crc`, set in
                PR_LoadProgs as CRC_Block((byte *)progs, fs_filesize).
                This is the identity of one exact build.  It is what
                retail_crcs[] holds, and it is the number uhexen2-qvqk wants.

  progdefs crc  The `crc` field of the dprograms_t header (offset 4), engine-
                side `progs->crc`.  This is the interface generation -- which
                globals layout the image expects -- and is what progdefs.h's
                PROGS_V103_CRC / PROGS_V111_CRC / PROGS_V112_CRC name.  Many
                different builds share one of these.

The distinction is the whole reason qvqk cannot be fixed by the progdefs CRC
alone: mods share progdefs CRCs with retail exactly.  Measured 2026-08-15,

    data1/PROGS.DAT      file 17499   progdefs 38488  (V111)
    GameOfTomes          file 41534   progdefs 38488  (V111)   <-- same
    portals/progs.dat    file 20799   progdefs 26905  (V112)
    karma2               file 22850   progdefs 26905  (V112)   <-- same
    soc                  file 58269   progdefs 26905  (V112)   <-- same
    sot                  file 28154   progdefs 26905  (V112)   <-- same

so "known Raven progdefs and no Hexenwail marker" would have misfiled all four
of those mods as retail.  Only the whole-file CRC separates them.

THE CHECKSUM

CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection, xorout 0x0000 --
common/crc.c, whose CRC_XOR_VALUE is 0x0000 rather than Quake's 0xffff.  The
table above is this script's output and matches the figures recorded in
docs/BUNDLED_GAMECODE.md, which is the check that it implements the same CRC
the engine does.
"""

import argparse
import struct
import sys

POLY = 0x1021
CRC_INIT_VALUE = 0xFFFF
CRC_XOR_VALUE = 0x0000

# progdefs.h; V109 is aliased to V111 there, so 38488 cannot tell them apart.
KNOWN_PROGDEFS = {
    14046: "v1.03",
    38488: "v1.09/v1.11",
    26905: "v1.12",
}

# PR_ClassifyGamecode()'s retail_crcs[], with the file each was measured from.
KNOWN_RETAIL = {
    17499: "retail data1/PROGS.DAT",
    20799: "retail portals/progs.dat",
    33075: "retail data1/PROGS2.DAT",
}


def _table():
    tab = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ POLY) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        tab.append(c)
    return tab


_TAB = _table()


def crc_block(data):
    """CRC_Block() from common/crc.c."""
    crc = CRC_INIT_VALUE
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _TAB[((crc >> 8) ^ b) & 0xFF]
    return crc ^ CRC_XOR_VALUE


def describe(path):
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 8:
        raise ValueError("too short to be a progs.dat")
    version, progdefs_crc = struct.unpack_from("<ii", data, 0)
    if version not in (6,):
        # Hexen II and Quake both use PROG_VERSION 6; anything else is not a
        # progs.dat and its "crc" field is meaningless.
        raise ValueError("header version %d, expected 6 -- not a progs.dat" % version)

    return crc_block(data), progdefs_crc


def main():
    ap = argparse.ArgumentParser(
        description="Print the file CRC and progdefs CRC of one or more progs.dat images.",
        epilog="The file CRC is what PR_ClassifyGamecode()'s retail_crcs[] table holds.",
    )
    ap.add_argument("progs", nargs="+", help="progs.dat / PROGS.DAT files")
    args = ap.parse_args()

    status = 0
    print("%-52s %8s %9s  %s" % ("file", "filecrc", "progdefs", "notes"))
    for path in args.progs:
        try:
            file_crc, progdefs_crc = describe(path)
        except (OSError, ValueError) as err:
            print("%-52s %8s %9s  %s" % (path, "-", "-", err))
            status = 1
            continue

        notes = []
        if file_crc in KNOWN_RETAIL:
            notes.append(KNOWN_RETAIL[file_crc])
        else:
            notes.append("file crc not in retail_crcs[]")
        notes.append("progdefs " + KNOWN_PROGDEFS.get(progdefs_crc, "unrecognised"))

        print("%-52s %8d %9d  %s" % (path, file_crc, progdefs_crc, "; ".join(notes)))

    return status


if __name__ == "__main__":
    sys.exit(main())
