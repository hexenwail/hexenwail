#!/usr/bin/env python3
"""
Fail the build when shipped progs.dat images disagree on the entity field set.

WHY THIS EXISTS (uhexen2-uhub, from uhexen2-acew)

A Hexen II savegame carries no fingerprint of the gamecode that wrote it.
Host_Loadgame_f validates SAVEGAME_VERSION and nothing else, and progs->crc is
never consulted on load.  Entity state is stored as field-NAME/value text and
restored through ED_ParseEdict -> ED_FindField, so a save crossed onto an image
with a different field set does not fail -- it loads, silently missing whatever
that image cannot place.  The engine now warns at load time, but a warning
after the fact is worse than never shipping the divergence at all.

So: compare the field tables, and refuse to build if they have drifted.

WHAT "AGREE" MEANS HERE

Identical tables, not merely compatible ones.  A superset would be tolerable in
one direction -- a save simply never references the new fields -- but the
offsets are what the engine indexes by, and adding a field renumbers everything
after it, so a superset in the name domain is usually a total mismatch in the
offset domain.  Requiring equality is both the honest check and the cheap one.

THE GOLDEN FILE

--golden pins the field set against a checked-in copy, so drift is caught even
though the one image that matters most -- Raven's retail PROGS.DAT -- cannot be
in the build.  It is unfree and lives only in the player's install.  As of
2026-08-15 the h2 golden is byte-for-byte what retail PROGS.DAT produces (497
fields, zero diff), which is what makes sv_gamecode 0 and 1 interchangeable
today.  Regenerating the golden is therefore a deliberate act: it declares that
saves written by earlier builds may lose state.

Usage:
  check_progs_fields.py [--golden FILE] <progs.dat> [<progs.dat> ...]
  check_progs_fields.py --write-golden FILE <progs.dat>

Exit status is 0 on agreement, 1 on divergence, 2 on a usage or read error.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from qcdis import Progs, ProgsError, TYPES, DEF_SAVEGLOBAL
except ImportError as e:
    print('check_progs_fields: cannot import qcdis: %s' % e, file=sys.stderr)
    sys.exit(2)


def fieldtable(path):
    """The image's entity fields as a list of "name type offset" lines.

    Same shape qcdis.py --fields prints, and deliberately so: when this fails,
    the first thing anyone will do is run qcdis by hand on the two images.
    """
    p = Progs(path)
    seen = {}
    for t, ofs, s in p.fielddefs:
        name = p.string(s)
        t &= ~DEF_SAVEGLOBAL
        # First def wins -- hcc can emit a name more than once, and this is the
        # rule the engine's ED_FindField follows.
        seen.setdefault(name, (t, ofs))
    out = []
    for name in sorted(seen):
        t, ofs = seen[name]
        tname = TYPES[t] if t < len(TYPES) else 'type%d' % t
        out.append('%-32s %-9s %d' % (name, tname, ofs))
    return out


def report(what_a, a, what_b, b):
    """Print the actual divergence.  A bare "they differ" is not actionable."""
    sa = {line.split()[0]: line for line in a}
    sb = {line.split()[0]: line for line in b}
    only_a = sorted(set(sa) - set(sb))
    only_b = sorted(set(sb) - set(sa))
    changed = sorted(n for n in set(sa) & set(sb) if sa[n] != sb[n])

    print('field set mismatch: %s vs %s' % (what_a, what_b), file=sys.stderr)
    print('  %d fields vs %d' % (len(a), len(b)), file=sys.stderr)
    for tag, names, src in (('only in %s' % what_a, only_a, sa),
                            ('only in %s' % what_b, only_b, sb)):
        if names:
            print('  %s (%d):' % (tag, len(names)), file=sys.stderr)
            for n in names[:20]:
                print('    %s' % src[n].rstrip(), file=sys.stderr)
            if len(names) > 20:
                print('    ... and %d more' % (len(names) - 20), file=sys.stderr)
    if changed:
        print('  type or offset changed (%d):' % len(changed), file=sys.stderr)
        for n in changed[:20]:
            print('    %s  ->  %s' % (sa[n].rstrip(), sb[n].rstrip()), file=sys.stderr)
        if len(changed) > 20:
            print('    ... and %d more' % (len(changed) - 20), file=sys.stderr)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Verify shipped progs images agree on the entity field set.')
    ap.add_argument('progs', nargs='+', help='progs.dat images to compare')
    ap.add_argument('--golden', metavar='FILE',
                    help='also require the field set to match this checked-in table')
    ap.add_argument('--write-golden', metavar='FILE',
                    help='write the first image\'s field table to FILE and exit')
    args = ap.parse_args(argv)

    try:
        tables = [(p, fieldtable(p)) for p in args.progs]
    except (ProgsError, OSError) as e:
        print('check_progs_fields: %s' % e, file=sys.stderr)
        return 2

    if args.write_golden:
        with open(args.write_golden, 'w') as f:
            f.write('\n'.join(tables[0][1]) + '\n')
        print('wrote %s (%d fields, from %s)'
              % (args.write_golden, len(tables[0][1]), tables[0][0]))
        return 0

    ok = True
    ref_path, ref = tables[0]
    for path, table in tables[1:]:
        if table != ref:
            report(ref_path, ref, path, table)
            ok = False

    if args.golden:
        try:
            with open(args.golden) as f:
                want = [ln.rstrip('\n') for ln in f if ln.strip()]
        except OSError as e:
            print('check_progs_fields: %s' % e, file=sys.stderr)
            return 2
        if ref != want:
            report(args.golden, want, ref_path, ref)
            print('\nIf this change is intended, regenerate with:\n'
                  '  tools/check_progs_fields.py --write-golden %s %s\n'
                  'and say in the commit message that saves from earlier builds\n'
                  'may silently lose the affected entity state.'
                  % (args.golden, ref_path), file=sys.stderr)
            ok = False

    if ok:
        print('progs field sets agree (%d fields across %d image%s)'
              % (len(ref), len(tables), '' if len(tables) == 1 else 's'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
