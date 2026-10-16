#!/usr/bin/env python3
"""
Convert a retail Hexen II install's .pak archives to .pk3 (zip) archives.

Usage:
  pak2pk3.py <gamedir> <outdir>            pak0.pak..pak9.pak -> 000-pakN.pk3
  pak2pk3.py <gamedir> <outdir> --loose    also pack the loose game files

Made for Raven's v1.11 data1 (pak0, pak1), the Nov. 1997 demo (data1/pak0),
Portal of Praevus (portals/pak3) and HexenWorld 0.14 - 0.15 (hw/pak4), the
archives the engine still recognises as a pk3; a mod's own paks convert only
with --force.
On a retail data1 the paks go from 98 MB to 46 MB.  GitHub #129.

WHAT IS PRESERVED, because the engine depends on it

- Members: same names, same order, same bytes, DEFLATE-compressed, and every
  one is read back and compared before the tool reports success.  Content
  identification matches on member sizes.
- DUPLICATE member names are kept, in pak order.  Retail data1/pak1.pak has
  three (models/mumshot.mdl, sound/mummy/mislfire.wav,
  sound/eidolon/flamend.wav).  Both readers in the engine index members in
  archive order with the same hash, so the same copy wins a lookup.
- Precedence against the paks it replaces: the outputs are named
  000-pak0.pk3, 000-pak1.pk3, ... so they keep pak1-over-pak0 and sort before
  the pk3s you are likely to have, which therefore still override them as
  they overrode the paks.  (The engine mounts pk3s in name order, last one
  wins, comparing with q_strcasecmp: ASCII case folded, then bytes as C
  chars, which are signed on every target that ships (x86, Apple arm64,
  WASM).  An existing pk3 that sorts below the converted archive it
  overrode -- below 000-pak1.pk3 for data1 -- would now lose to the base
  content: for example any name starting with a space or one of
  !"#$%&'()*+,-./, "00" then a byte below "0", "000-" then anything below
  "pak1" (000-hd.pk3), and, where char is signed, a non-ASCII first byte.
  The tool compares every pk3 already in the output directory and names each
  one whose rank changes, including one that would now beat zz-loose.pk3.)
- Precedence against paks you KEEP: none can be kept.  Every pk3 outranks
  every pak, so converting data1's pak0/pak1 while a mod's pak5.pak stays
  would put the base content above the mod.  A gamedir is converted whole,
  and a pak the tool will not convert stops the run.

WHAT IS REFUSED, and why

- A data1 with neither a v1.11 pak0 and pak1 pair nor the Nov. 1997 demo's
  pak0: the OEM release, pre-1.11 installs, the older 0.42 demo.  As .pk3s
  the engine identifies data1 only from those, and a converted one fails
  with "Unable to find a proper Hexen II installation".  A portals without
  the v1.11 pak3 likewise.  Not overridable.
- On the demo's data1, --loose and any pak beside its pak0.  The demo runs
  no modified content, and every pk3 that is not a whole Raven pak counts
  as modified.  Not overridable.
- An hw without the HexenWorld 0.14 - 0.15 pak4.pak (0.09 and 0.11 have no
  content marks).  As a .pk3 hwsv would not recognise it and stops with "You
  must have the HexenWorld data installed".  Not overridable.
- Any other pak beside the base content (a mod's pak5.pak), and a mod's own
  gamedir, unless --force.  Those mount as unrecognised archives, which marks
  the game "modified" (fine on a registered install), and since every pk3
  outranks every pak a gamedir has to be converted whole.
- A result over 64 pk3s in one directory (MAX_PK3_PER_DIR), counting the
  ones already there.  Past the limit the engine drops archives in readdir
  order, the converted base content included, and the install would not
  start (measured by a reviewer: 000-pak0.pk3 was the one dropped).
- A run with nothing to convert.
- Member names that are not ASCII: Python's zip writer would re-encode them
  as UTF-8 and the engine, which compares bytes, would no longer find them.
- A pak over 2048 members (MAX_FILES_IN_PACK), which the engine never mounts.
- Files other than pak0.pak..pak9.pak: the engine opens only those, so
  converting a pak0_old.pak would bring back an archive it was ignoring.  The
  match is exact where the filesystem is case-sensitive (Linux) and folds
  case where it is not (Windows, macOS), as the engine's own fopen does.

LOOSE FILES

A retail install keeps game files OUTSIDE its paks:
  data1:    PROGS.DAT, PROGS2.DAT, Strings.txt, Hexen.rc
  portals:  progs.dat, strings.txt, puzzles.txt, infolist.txt, default.cfg,
            hexen.rc
Delete them and the game breaks in ways that say nothing about packing:
without infolist.txt the mission pack stops at map load ("couldn't load
infolist.txt"), and a build without bundled gamecode has none ("PR_LoadProgs:
couldn't load progs.dat").  Keep them, or pass --loose, which packs exactly
the files named in LOOSE_GAME_FILES below, from the gamedir's top level only,
into zz-loose.pk3, and names every other top-level file and subdirectory it
leaves behind.

zz-loose.pk3 sorts after the converted paks and after most pk3s, but not
all: loose files used to beat every archive, and a pk3 named after
"zz-loose" (zz_*.pk3, zzz*.pk3) would now override them.  Savegames, demos,
screenshots, config.cfg, autoexec.cfg and anything in a subdirectory are
never packed; autoexec.cfg is named among the files left behind, since it is
usually the player's own.

Write the output into the same gamedir it came from.  A userdir's gamedir
mounts above the basedir's, so pk3s written to the userdir instead would
outrank the basedir's own loose files and pk3s.

Nothing is written in place: each archive is built under a temporary name and
renamed only once verified, and the source files are never modified.  Remove
the .pak files yourself once the .pk3s load; until then both mount and the
pk3 wins.

Every pak is read into memory before anything is written (about 200 MB
peak for a retail data1).  Stock python3 only.
"""
import argparse
import os
import struct
import sys
import tempfile
import warnings
import zipfile

# quakefs.c MAX_ZIP_INFLATE: a DEFLATEd member that inflates past this is
# skipped by the engine, so such members are written STORED instead.
MAX_ZIP_INFLATE = 64 * 1024 * 1024
# common/pakfile.h: a pak with more members is refused by the engine.
MAX_FILES_IN_PACK = 2048
# quakefs.c: pk3s mounted per directory; past it the engine drops the rest.
MAX_PK3_PER_DIR = 64
PAK_NAMES = ["pak%d.pak" % i for i in range(10)]
OUT_PREFIX = "000-"
LOOSE_ARCHIVE = "zz-loose.pk3"
# The loose files a retail install keeps beside its paks.
LOOSE_GAME_FILES = {"progs.dat", "progs2.dat", "hwprogs.dat", "strings.txt",
                    "puzzles.txt", "maplist.txt", "infolist.txt", "hexen.rc",
                    "default.cfg"}
# Top-level files never packed and not worth a warning: archives, and what
# the engine or the player writes.
NOT_GAME_FILES = (".pak", ".pk3", ".dem", ".log", ".png", ".tga", ".pcx")
CASE_INSENSITIVE_FS = sys.platform in ("win32", "darwin")

# quakefs.c contentdata[]: the member sizes FS_IdentifyContent matches to
# recognise Raven's content inside an archive, and the gamedir each belongs
# in.  tools/test_pak2pk3.py checks this copy against quakefs.c.
CONTENT_MARKS = {
    # First: the demo's pak0 carries every "data1 pak0" mark too, and
    # identify() takes the first key that matches.
    "data1 demo": ("data1", {
        "strings.txt": 16539, "models/h_cru.mdl": 16896,
        "models/h_nec.mdl": 8052, "models/castrch.mdl": 16996,
        "gfx/menu/netp2.lmp": 7760}),
    "data1 pak0": ("data1", {
        "maplist.txt": 193, "puzzles.txt": 1131, "default.cfg": 2793,
        "gfx.wad": 10732, "gfx/palette.lmp": 768, "maps/demo1.bsp": 1896452}),
    "data1 pak1": ("data1", {
        "gfx/pop.lmp": 256, "maps/eidolon.bsp": 528796,
        "maps/cath.bsp": 2013328, "maps/egypt1.bsp": 2338472}),
    "portals": ("portals", {
        "default.cfg": 3164, "maps/keep1.bsp": 2500124,
        "maps/tibet1.bsp": 2463304, "maps/tibet7.bsp": 2749412}),
    "hw pak4": ("hw", {
        "maps/hwdm1.bsp": 1066196, "maps/hwdm3.bsp": 620284,
        "maps/hwdm5.bsp": 1521668, "sound/taunt/taunt001.wav": 9996,
        "models/h_suc.mdl": 6268}),
}


class PakError(Exception):
    pass


def is_pk3(name):
    """Would the engine's "*.pk3" glob mount it?  The match is
    case-sensitive on Linux and folds case on Windows and macOS."""
    return name.lower().endswith(".pk3") if CASE_INSENSITIVE_FS else name.endswith(".pk3")


def engine_key(name):
    """q_strcasecmp order: ASCII case folded, bytes as signed chars."""
    raw = os.fsencode(name).lower()
    return [b - 256 if b >= 128 else b for b in raw]


def precedence_warnings(existing, converted, loose_archive):
    """Names in `existing` whose rank against the base content changes.

    Before: every pk3 beat every pak, and loose files beat every pk3.  After:
    the converted archives sit in the pk3 order, and the loose files in
    loose_archive (if any) do too."""
    out = []
    top = max((engine_key(n) for n in converted), default=None)
    for n in sorted(existing, key=engine_key):
        if top is not None and engine_key(n) < top:
            out.append("%s sorts below %s, so it no longer overrides the base "
                       "content it used to; rename it to sort after %s"
                       % (printable(n), max(converted, key=engine_key),
                          max(converted, key=engine_key)))
        if loose_archive and engine_key(n) > engine_key(loose_archive):
            out.append("%s sorts after %s, so it now overrides the formerly "
                       "loose files" % (printable(n), loose_archive))
    return out


def printable(name):
    """A directory entry that is not valid UTF-8 must not crash a print."""
    return name.encode("utf-8", "surrogateescape").decode("utf-8", "replace")


def read_pak(path):
    """[(name, bytes)] in directory order, duplicates included."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 12 or data[:4] != b"PACK":
        raise PakError("%s: not a pak file" % path)
    dirofs, dirlen = struct.unpack("<ii", data[4:12])
    if dirofs < 12 or dirlen < 0 or dirofs + dirlen > len(data):
        raise PakError("%s: corrupt directory (ofs %d, len %d, file %d bytes)"
                       % (path, dirofs, dirlen, len(data)))
    count = dirlen // 64      # the engine rounds a ragged directory down too
    if count > MAX_FILES_IN_PACK:
        raise PakError("%s: %d members, over the engine's %d; it never mounts "
                       "this pak" % (path, count, MAX_FILES_IN_PACK))
    members = []
    for i in range(count):
        entry = data[dirofs + i * 64:dirofs + (i + 1) * 64]
        raw = entry[:56].split(b"\0", 1)[0]
        ofs, size = struct.unpack("<ii", entry[56:64])
        if ofs < 0 or size < 0 or ofs + size > len(data):
            raise PakError("%s: member %r runs past the end" % (path, raw))
        try:
            name = raw.decode("ascii")
        except UnicodeDecodeError:
            raise PakError("%s: member name %r is not ASCII; a zip would "
                           "re-encode it and the engine would not find it"
                           % (path, raw)) from None
        if not name or name.endswith("/"):
            raise PakError("%s: member name %r is not a file name" % (path, name))
        members.append((name, data[ofs:ofs + size]))
    return members


def identify(members):
    """The CONTENT_MARKS key these members carry, or None."""
    # The engine looks marks up through its hash, where the LAST copy of a
    # duplicated name wins, so the last size is the one that counts.
    sizes = {}
    for name, blob in members:
        sizes[name.lower()] = len(blob)
    for key, (_, marks) in CONTENT_MARKS.items():
        if all(sizes.get(n) == s for n, s in marks.items()):
            return key
    return None


def write_pk3(members, out):
    """Write and verify under a temporary name, then rename.
    Returns the number of duplicate names kept."""
    seen, dups = set(), 0
    fd, tmp = tempfile.mkstemp(prefix=".pak2pk3-", suffix=".tmp",
                               dir=os.path.dirname(out) or ".")
    os.close(fd)
    try:
        with warnings.catch_warnings():
            # zipfile warns on a repeated name.  Repeats are the point here.
            warnings.filterwarnings("ignore", message="Duplicate name")
            with zipfile.ZipFile(tmp, "w") as z:
                for name, blob in members:
                    if name.lower() in seen:
                        dups += 1
                    seen.add(name.lower())
                    info = zipfile.ZipInfo(name, date_time=(1997, 1, 1, 0, 0, 0))
                    info.external_attr = 0o644 << 16
                    if len(blob) > MAX_ZIP_INFLATE:
                        info.compress_type = zipfile.ZIP_STORED
                        z.writestr(info, blob)
                    else:
                        info.compress_type = zipfile.ZIP_DEFLATED
                        z.writestr(info, blob, compresslevel=9)
        verify_pk3(members, tmp)
        # mkstemp makes the file 0600, and a rename keeps the mode: an
        # install owned by one account and run by another (a server's service
        # user, data converted under sudo) could not open the result, and the
        # engine's pk3 loader fails silently.  Use what open() would give.
        os.chmod(tmp, 0o666 & ~current_umask())
        os.replace(tmp, out)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return dups


def current_umask():
    mask = os.umask(0)
    os.umask(mask)
    return mask


def verify_pk3(members, out):
    """Every member, in order, byte for byte, names as raw bytes."""
    with zipfile.ZipFile(out) as z:
        infos = z.infolist()
        if len(infos) != len(members):
            raise PakError("%s: %d members written, %d expected"
                           % (out, len(infos), len(members)))
        for info, (name, blob) in zip(infos, members):
            if info.flag_bits & 0x800 or info.orig_filename != name \
                    or info.file_size != len(blob):
                raise PakError("%s: member %s: written as %r (%d bytes)"
                               % (out, name, info.orig_filename, info.file_size))
            if z.open(info).read() != blob:
                raise PakError("%s: member %s: bytes differ" % (out, name))


def loose_members(gamedir):
    members = []
    for fn in sorted(os.listdir(gamedir)):
        full = os.path.join(gamedir, fn)
        if fn.lower() in LOOSE_GAME_FILES and os.path.isfile(full):
            if not fn.isascii():
                raise PakError("%s: name is not ASCII" % full)
            with open(full, "rb") as f:
                members.append((fn, f.read()))
    return members


def report(label, members, out, dups):
    size_in = sum(len(b) for _, b in members)
    size_out = os.path.getsize(out)
    line = ("%-14s %5d members%s, %6.1f MB -> %6.1f MB  %s"
            % (label, len(members),
               " (%d duplicate name%s kept)" % (dups, "" if dups == 1 else "s") if dups else "",
               size_in / 1e6, size_out / 1e6, printable(out)))
    return size_in, size_out, line


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Convert a retail Hexen II gamedir's .pak archives to .pk3.",
        epilog="See the module docstring or tools/README.md for what is "
               "preserved and what is refused.")
    ap.add_argument("gamedir", help="data1, portals or hw of a retail install")
    ap.add_argument("outdir", help="where to write the .pk3 files (created if missing)")
    ap.add_argument("--loose", action="store_true",
                    help="also pack the loose game files into %s" % LOOSE_ARCHIVE)
    ap.add_argument("--force", action="store_true",
                    help="also convert paks that are not Raven's v1.11 "
                         "content: a mod's pak5 beside the base game, or a "
                         "mod's own gamedir.  They mount as unrecognised "
                         "archives, which only a registered install may run.  "
                         "Never overrides the data1, portals or hw rules.")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.gamedir):
        print("error: %s is not a directory" % args.gamedir, file=sys.stderr)
        return 1
    # abspath, not realpath: "." must name the directory, and a symlinked
    # data1 must keep its own name.
    gamedir_name = os.path.basename(os.path.abspath(args.gamedir)).lower()

    try:
        present = os.listdir(args.gamedir)
        if CASE_INSENSITIVE_FS:
            by_lower = {n.lower(): n for n in present}
            paks = [by_lower[n] for n in PAK_NAMES if n in by_lower]
        else:
            paks = [n for n in PAK_NAMES if n in present]
        ignored = sorted(n for n in present
                         if n.lower().endswith(".pak") and n not in paks)
        for n in ignored:
            print("skipped %s: the engine opens only pak0.pak..pak9.pak"
                  % printable(n))

        if gamedir_name not in ("data1", "portals", "hw") and not args.force:
            hint = ""
            if any(os.path.isdir(os.path.join(args.gamedir, d)) and d.lower() == "data1"
                   for d in present):
                hint = ("  It has a data1 inside: this looks like the install "
                        "root, so pass %s instead."
                        % printable(os.path.join(args.gamedir, "data1")))
            raise PakError(
                "%s is not data1, portals or hw, so this is a mod's directory (or "
                "the wrong one).  Rerun with --force to convert it: its pk3s "
                "mount as unrecognised archives, which only a registered "
                "install may run.%s" % (printable(args.gamedir), hint))

        if not paks and not args.loose:
            print("error: no pak0.pak..pak9.pak in %s" % args.gamedir, file=sys.stderr)
            return 1

        # Read and identify everything before writing anything.
        plan = []
        for name in paks:
            members = read_pak(os.path.join(args.gamedir, name))
            plan.append((name, members, identify(members)))

        def base_ok(key):
            return key is not None and CONTENT_MARKS[key][0] == gamedir_name

        # The base content first, because it decides whether anything here
        # can be converted at all, and neither rule can be overridden: a
        # gamedir converted without it never starts.
        #
        # FS_Init counts a data1 as an installation with BOTH the pak0 and
        # the pak1 content (GAME_REGISTERED0 | GAME_REGISTERED1), or with the
        # Nov. 1997 demo's pak0 (GAME_DEMO, whose row in contentdata[] takes
        # back the GAME_REGISTERED0 its pak0 also matches).  Anything else, as
        # a .pk3, fails: "Unable to find a proper Hexen II installation".
        # OEM, pre-1.11 and 0.42-demo data1 paks carry no marks.
        # An empty plan is --loose on a gamedir already converted: fine.
        found = {key for _, _, key in plan if base_ok(key)}
        if plan and gamedir_name == "data1" and \
                not {"data1 pak0", "data1 pak1"} <= found and \
                "data1 demo" not in found:
            raise PakError(
                "%s has no v1.11 pak0.pak and pak1.pak pair, and no Nov. 1997 "
                "demo pak0.pak (the OEM release, pre-1.11 installs and the "
                "older demo have neither).  As .pk3s the engine identifies "
                "data1 only from those, and would refuse to start: \"Unable "
                "to find a proper Hexen II installation\".  Leave this data1 "
                "as paks." % args.gamedir)
        if plan and gamedir_name == "portals" and "portals" not in found:
            raise PakError(
                "%s has no v1.11 Portal of Praevus pak3.pak.  As a .pk3 the "
                "engine would not recognise the mission pack.  Leave it as "
                "paks." % args.gamedir)
        # hwsv requires GAME_HEXENWORLD, and as a .pk3 only the 0.14 - 0.15
        # pak4 carries marks for it.  glhexen2 mounts hw only to join a
        # HexenWorld server, and identifies nothing there.
        if plan and gamedir_name == "hw" and "hw pak4" not in found:
            raise PakError(
                "%s has no HexenWorld 0.14 - 0.15 pak4.pak (0.09 and 0.11 "
                "have no content marks).  As a .pk3 hwsv would not recognise "
                "it: \"You must have the HexenWorld data installed\".  Leave "
                "it as paks." % args.gamedir)
        # The demo cannot run modified content (FS_Init: "You must have the
        # full version of Hexen II to play modified games"), and every pk3
        # but a whole Raven pak repacked counts as modified: a zz-loose.pk3,
        # or a converted mod pak, would stop it starting.  Not overridable.
        if "data1 demo" in found:
            if args.loose:
                raise PakError(
                    "%s is the demo, which runs no modified content, and %s "
                    "would be an archive Raven never shipped: the demo would "
                    "refuse to start (\"You must have the full version of "
                    "Hexen II to play modified games\").  Rerun without "
                    "--loose; the loose files work as they are."
                    % (printable(args.gamedir), LOOSE_ARCHIVE))
            other = [name for name, _, key in plan if not base_ok(key)]
            if other:
                raise PakError(
                    "%s is the demo, which runs no modified content, and %s "
                    "%s not Raven's.  Converted or not, the demo would refuse "
                    "to start with %s installed.  Move %s out of %s."
                    % (printable(args.gamedir), ", ".join(other),
                       "is" if len(other) == 1 else "are",
                       "it" if len(other) == 1 else "them",
                       "it" if len(other) == 1 else "them",
                       printable(args.gamedir)))

        # Whatever else is here: a mod's pak5 beside the base content, or a
        # mod's own gamedir.  Converting is fine -- it mounts as an
        # unrecognised archive, which marks the game modified, and a
        # registered install runs modified games -- but it must be all or
        # nothing, since a pk3 outranks every pak.
        # (A mod's own gamedir was already refused above without --force, so
        # an extra pak reaching here sits beside recognised base content.)
        extra = [name for name, _, key in plan if not base_ok(key)]
        if extra and not args.force:
            base = [name for name, _, key in plan if base_ok(key)]
            them = "it" if len(extra) == 1 else "them"
            raise PakError(
                "%s %s not Raven's v1.11 content.  Converting only %s would "
                "put the base content ABOVE %s, since every pk3 outranks "
                "every pak.  Rerun with --force to convert %s too (as an "
                "unrecognised archive, which is fine on a registered "
                "install), or move %s out of %s first."
                % (", ".join(extra), "is" if len(extra) == 1 else "are",
                   ", ".join(base), them, them, them, printable(args.gamedir)))
        for name, _, key in plan:
            if not base_ok(key):
                what = ("is %s content, which belongs in %s, not %s"
                        % (key, CONTENT_MARKS[key][0], gamedir_name)
                        if key else "is not Raven's v1.11 content")
                print("warning: %s %s; converting it as an unrecognised "
                      "archive because of --force" % (name, what))

        loose = loose_members(args.gamedir) if args.loose else []

        # Existing pk3s in the output directory whose rank against the base
        # content changes, and outputs that would replace a file already
        # there: both said before anything is written.
        outputs = [OUT_PREFIX + name[:-4].lower() + ".pk3" for name, _, _ in plan]
        if loose:
            outputs.append(LOOSE_ARCHIVE)
        if not outputs:
            raise PakError("nothing to convert in %s: no pak0.pak..pak9.pak and "
                           "no loose game files" % printable(args.gamedir))
        existing = os.listdir(args.outdir) if os.path.isdir(args.outdir) else []
        # The outputs belong in the gamedir, so its pk3s are the ones they
        # will sit beside, wherever this run writes them.
        staged = os.path.abspath(args.outdir) != os.path.abspath(args.gamedir)
        neighbours = set(existing) | (set(present) if staged else set())
        neighbours = sorted(n for n in neighbours if is_pk3(n) and n not in outputs)

        # FS_AddGameDirectory keeps only MAX_PK3_PER_DIR pk3s, chosen in
        # readdir order BEFORE sorting, so past the limit which ones are
        # dropped is arbitrary -- and can be the converted base content, which
        # leaves an install that will not start.
        if len(neighbours) + len(outputs) > MAX_PK3_PER_DIR:
            raise PakError(
                "%s would hold %d pk3s (%d already there, %d from this run), "
                "over the engine's %d per directory.  Past that limit it drops "
                "archives arbitrarily, the converted base content included, "
                "and the install would not start.  Merge or remove some of "
                "the existing pk3s first."
                % (printable(args.gamedir), len(neighbours) + len(outputs),
                   len(neighbours), len(outputs), MAX_PK3_PER_DIR))

        lines = []
        if staged:
            lines.append("note: writing to %s, not the gamedir; the warnings "
                         "below compare against the pk3s in both, since the "
                         "outputs belong beside %s's"
                         % (printable(args.outdir), printable(args.gamedir)))
        lines += ["warning: " + w for w in precedence_warnings(
            neighbours, [n for n in outputs if n != LOOSE_ARCHIVE],
            LOOSE_ARCHIVE if loose else None)]
        lines += ["note: replacing the existing %s" % printable(n)
                  for n in outputs if n in existing]

        # Write everything, then report: nothing below may fail between two
        # writes and leave a half-converted directory.
        os.makedirs(args.outdir, exist_ok=True)
        total_in = total_out = 0
        for name, members, _ in plan:
            out = os.path.join(args.outdir, OUT_PREFIX + name[:-4].lower() + ".pk3")
            dups = write_pk3(members, out)
            i, o, line = report(name[:-4], members, out, dups)
            lines.append(line)
            total_in, total_out = total_in + i, total_out + o
        if loose:
            out = os.path.join(args.outdir, LOOSE_ARCHIVE)
            i, o, line = report("loose files", loose, out, write_pk3(loose, out))
            lines.append(line)
            total_in, total_out = total_in + i, total_out + o
            lines += ["  loose: %s" % rel for rel, _ in loose]
        if args.loose:
            packed = {rel for rel, _ in loose}
            left = sorted(n for n in present
                          if n not in packed and os.path.isfile(os.path.join(args.gamedir, n))
                          and not n.lower().endswith(NOT_GAME_FILES)
                          and n.lower() != "config.cfg")
            lines += ["  not packed, keep it if the game needs it: %s" % printable(n)
                      for n in left]
            subdirs = sorted(n for n in present if os.path.isdir(os.path.join(args.gamedir, n)))
            if subdirs:
                lines.append("  subdirectories are never packed: %s"
                             % ", ".join(printable(n) for n in subdirs))
        else:
            kept = sorted(n for n in present if n.lower() in LOOSE_GAME_FILES)
            if kept:
                lines.append("note: %s stay loose in %s; the game needs them, so "
                             "keep them, or rerun with --loose"
                             % (", ".join(printable(n) for n in kept),
                                printable(args.gamedir)))
        lines.append("total %.1f MB -> %.1f MB" % (total_in / 1e6, total_out / 1e6))
        print("\n".join(lines))
    except (PakError, OSError, UnicodeError) as err:
        print("error: %s" % err, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
