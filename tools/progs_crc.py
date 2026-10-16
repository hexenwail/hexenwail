#!/usr/bin/env python3
"""
Print the two CRCs PR_ClassifyGamecode() identifies a progs.dat by.

WHY THIS EXISTS (uhexen2-qvqk)

PR_ClassifyGamecode() (engine/h2shared/pr_edict.c) names Raven's gamecode from
pr_raven_images[], a table of (whole-file CRC, progdefs CRC, release) rows.  It
first held only the three images measured off one retail install (1.11 and
the CD's portals), so a player on 1.03, 1.08, 1.09, 1.10 or the 1.12a update
saw their retail gamecode called "Third-party" (GitHub #160).  The rest of the
table came from running this script over sezero's uhexen2-hcode_archive (see
KNOWN_RAVEN and ARCHIVE_ROWS below).

Reading a CRC off the engine's own "Gamecode:" line means owning that release
and running it.  This reads both straight out of the file instead, so anyone
who can put a progs.dat in front of a shell can report the values without
installing anything.

WHAT THE TWO NUMBERS ARE

  file crc      CRC over the entire file, engine-side `pr_crc`, set in
                PR_LoadProgs as CRC_Block((byte *)progs, fs_filesize).
                This is the identity of one exact build.

  progdefs crc  The `crc` field of the dprograms_t header (offset 4), engine-
                side `progs->crc`.  This is the interface generation -- which
                globals layout the image expects -- and is what progdefs.h's
                PROGS_V103_CRC / PROGS_V111_CRC / PROGS_V112_CRC name.  Many
                different builds share one of these.

The progdefs CRC cannot do the job alone, for two reasons.  Releases share it:
1.08 through 1.11 all declare 38488, and portals 1.12 and 1.12a both declare
26905.  And mods share it with retail exactly.  Measured 2026-08-15,

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
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from qcdis import Progs, ProgsError
except ImportError as e:
    print('progs_crc: cannot import qcdis: %s' % e, file=sys.stderr)
    sys.exit(2)

POLY = 0x1021
CRC_INIT_VALUE = 0xFFFF
CRC_XOR_VALUE = 0x0000

# progdefs.h; V109 is aliased to V111 there, so 38488 cannot tell them apart.
KNOWN_PROGDEFS = {
    14046: "v1.03",
    38488: "v1.09/v1.11",
    26905: "v1.12",
}

# pr_edict.c's pr_raven_images[]: (file crc, progdefs crc) -> (release the
# engine prints, which file it is).  tools/test_progs_release_ident.py fails
# if the two tables drift apart.
KNOWN_RAVEN = {
    (20607, 14046): ("Raven 1.03", "data1/progs.dat"),
    (60119, 14046): ("Raven 1.03", "data1/progs2.dat"),
    (1534, 38488): ("Raven 1.08", "data1/progs.dat"),
    (17570, 38488): ("Raven 1.08", "data1/progs2.dat"),
    (30965, 38488): ("Raven 1.09", "data1/progs.dat"),
    (50972, 38488): ("Raven 1.09", "data1/progs2.dat"),
    (1209, 38488): ("Raven 1.10", "data1/progs.dat"),
    (32649, 38488): ("Raven 1.10", "data1/progs2.dat"),
    (17499, 38488): ("Raven 1.11", "data1/PROGS.DAT"),
    (33075, 38488): ("Raven 1.11", "data1/PROGS2.DAT"),
    (20799, 26905): ("Raven 1.12", "portals/progs.dat, CD release"),
    (3174, 26905): ("Raven 1.12a", "portals/progs.dat, official update"),
    (16640, 38488): ("Raven demo", "data1/progs.dat, Nov 1997 demo"),
}

# Where each row came from.  "measured" means read off a real install or the
# hash-verified .#demodata; "archive" means sezero's uhexen2-hcode_archive
# bin/ images, whose 1.11 and 1.12 files are byte-identical to the measured
# retail ones.  Not every bin/ image there is a shipped binary: its
# h2-1.11-demo (crc 47044) is not the real demo (16640) and looks rebuilt, so
# an archive row is good evidence, not proof.  tools/test_progs_release_ident.py
# re-reads the archive rows when HCODE_ARCHIVE points at a checkout.
ARCHIVE_ROWS = {
    (20607, 14046): "h2-1.03/bin/progs.dat",
    (60119, 14046): "h2-1.03/bin/progs2.dat",
    (1534, 38488): "h2-1.08/bin/progs.dat",
    (17570, 38488): "h2-1.08/bin/progs2.dat",
    (30965, 38488): "h2-1.09/bin/progs.dat",
    (50972, 38488): "h2-1.09/bin/progs2.dat",
    (1209, 38488): "h2-1.10/bin/progs.dat",
    (32649, 38488): "h2-1.10/bin/progs2.dat",
    (17499, 38488): "h2-1.11/bin/progs.dat",
    (33075, 38488): "h2-1.11/bin/progs2.dat",
    (20799, 26905): "portals-1.12/bin/progs.dat",
    (3174, 26905): "portals-1.12a/bin/progs.dat",
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
    # qcdis.Progs is the repo's one progs.dat header parser -- it unpacks the
    # full dprograms_t, checks the version and bounds-checks every section, and
    # keeps the raw bytes as .buf, which is exactly what the whole-file CRC
    # needs.  Parsing the header a second time here would only give the two
    # tools different ideas of what counts as a progs.dat.
    p = Progs(path)
    return crc_block(p.buf), p.crc


def main():
    ap = argparse.ArgumentParser(
        description="Print the file CRC and progdefs CRC of one or more progs.dat images.",
        epilog="PR_ClassifyGamecode()'s pr_raven_images[] matches on both CRCs.",
    )
    ap.add_argument("progs", nargs="+", help="progs.dat / PROGS.DAT files")
    args = ap.parse_args()

    status = 0
    print("%-52s %8s %9s  %s" % ("file", "filecrc", "progdefs", "notes"))
    for path in args.progs:
        try:
            file_crc, progdefs_crc = describe(path)
        except (OSError, ProgsError) as err:
            # qcdis prefixes its messages with the path; the row already has a
            # path column, so drop the repeat.
            msg = str(err)
            if msg.startswith(path + ": "):
                msg = msg[len(path) + 2:]
            print("%-52s %8s %9s  %s" % (path, "-", "-", msg))
            status = 1
            continue

        notes = []
        known = KNOWN_RAVEN.get((file_crc, progdefs_crc))
        if known:
            notes.append("%s (%s)" % known)
        else:
            notes.append("not a known Raven image")
        notes.append("progdefs " + KNOWN_PROGDEFS.get(progdefs_crc, "unrecognised"))

        print("%-52s %8d %9d  %s" % (path, file_crc, progdefs_crc, "; ".join(notes)))

    return status


if __name__ == "__main__":
    sys.exit(main())
