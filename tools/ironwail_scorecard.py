#!/usr/bin/env python3
"""Recount the ARE_WE_IRONWAIL_YET.md scorecard from the tables themselves.

The document records, twice, that hand-summarising a table you have just
edited does not work: the first draft of the scorecard came out 87/2/20/9
against an actual 87/1/24/9, and before that the table had drifted to
85/2/1/9.  Both times the fix was to parse the status column instead of
counting by eye, and both times the parser was thrown away afterwards.
This is that parser, kept.

    python3 tools/ironwail_scorecard.py          # print the recount
    python3 tools/ironwail_scorecard.py --check  # exit 1 if the table disagrees

Every `## Section` after the Scorecard owns the rows beneath it; a row counts
when its second cell is exactly one of the four status glyphs, which is what
keeps header and separator rows out of the tally.
"""

import argparse
import collections
import io
import os
import re
import sys

DOC = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   os.pardir, "history", "ARE_WE_IRONWAIL_YET.md")
STATUSES = ("✅", "\U0001f536", "❌", "➖")   # ported, partial, missing, N/A


def tally(text):
    counts = collections.OrderedDict()
    section = None
    in_scorecard = False
    for line in text.split("\n"):
        if line.startswith("## "):
            section = line[3:].strip()
            in_scorecard = (section == "Scorecard")
            continue
        if in_scorecard or section is None or not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3 or cells[1] not in STATUSES:
            continue
        counts.setdefault(section, collections.Counter())[cells[1]] += 1
    return counts


def stated(text):
    """The counts the Scorecard table currently claims, for --check."""
    claimed = collections.OrderedDict()
    for line in text.split("\n"):
        m = re.match(r"^\|\s*(?:\*\*)?([^|*]+?)(?:\*\*)?\s*\|"
                     + r"\s*(?:\*\*)?(\d+)(?:\*\*)?\s*\|" * 4 + r"\s*$", line)
        if m:
            claimed[m.group(1).strip()] = tuple(int(m.group(i)) for i in range(2, 6))
    return claimed


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if the Scorecard table disagrees with the rows")
    ap.add_argument("--doc", default=DOC)
    args = ap.parse_args()

    text = io.open(args.doc, encoding="utf-8").read()
    counts = tally(text)
    total = collections.Counter()

    print("%-32s %4s %4s %4s %4s" % ("Category", *STATUSES))
    for section, c in counts.items():
        print("%-32s %4d %4d %4d %4d" % (section, *(c[s] for s in STATUSES)))
        total.update(c)
    print("%-32s %4d %4d %4d %4d" % ("TOTAL", *(total[s] for s in STATUSES)))

    den = sum(total[s] for s in STATUSES[:3])          # N/A is out of the denominator
    print("\nParity: %.0f%% ported (%d/%d), %.0f%% partial, %.0f%% missing"
          % (100.0 * total[STATUSES[0]] / den, total[STATUSES[0]], den,
             100.0 * total[STATUSES[1]] / den, 100.0 * total[STATUSES[2]] / den))

    if not args.check:
        return 0

    claimed = stated(text)
    bad = []
    for section, c in counts.items():
        want = tuple(c[s] for s in STATUSES)
        if claimed.get(section) != want:
            bad.append((section, claimed.get(section), want))
    want_total = tuple(total[s] for s in STATUSES)
    if claimed.get("TOTAL") != want_total:
        bad.append(("TOTAL", claimed.get("TOTAL"), want_total))

    for section, got, want in bad:
        print("MISMATCH %-28s table says %s, rows say %s" % (section, got, want),
              file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
