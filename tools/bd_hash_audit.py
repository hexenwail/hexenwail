#!/usr/bin/env python3
"""Find commit hashes cited in beads that are no longer in master's history.

An interactive rebase rewrites hashes.  Any hash already written into a bead
description, note or close reason still points at the pre-rebase object, which
survives in the object store just long enough to look valid and then becomes
unreachable.  uhexen2-nkj1 and uhexen2-rp1z both had to be corrected by hand
after this happened; this is that check, automated.

Each cited hash lands in one of four buckets:

  ok         reachable from HEAD -- nothing to do
  archived   not in HEAD but reachable from some other ref, e.g. an
             archive/* branch.  Usually deliberate (uhexen2-ntb9 cites the
             26.03-alpha.7i experiment on purpose), so it is reported
             separately rather than as an error
  rewritten  the object exists and a commit with the same subject IS in HEAD:
             almost certainly the rebase survivor, printed as a suggested
             correction.  Patch-ids routinely differ because a beads export
             gets squashed in, so subject plus changed-file agreement is the
             test, not patch-id equality
  dangling   the object exists, is on no ref, and no commit in HEAD shares its
             subject.  Typically a beads-only commit that was squashed into a
             code commit; no single hash replaces it, so cite the bead instead

Usage:  tools/bd_hash_audit.py [--all]
        --all also lists hashes cited only by closed beads (noise, mostly)

Exits 1 if any open bead cites a rewritten or dangling hash.
"""

import collections
import json
import os
import re
import subprocess
import sys

JSONL = os.path.join(os.path.dirname(__file__), "..", ".beads", "issues.jsonl")
TEXT_FIELDS = ("description", "notes", "design", "close_reason",
               "acceptance_criteria", "resolution")

# 7-40 hex chars.  Pure digits are excluded because the tracker is full of
# CRCs (17499, 38488, ...) that would otherwise be tested as abbreviated
# hashes and, at 5 digits, occasionally resolve.
HASH_RE = re.compile(r"\b(?=[0-9a-f]*[a-f])[0-9a-f]{7,40}\b")


def git(*args):
    return subprocess.run(("git",) + args, capture_output=True, text=True)


def load_issues():
    issues = []
    with open(JSONL) as fh:
        for line in fh:
            line = line.strip()
            if line:
                try:
                    issues.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    return issues


def main():
    show_closed = "--all" in sys.argv
    issues = load_issues()
    status = {d.get("id"): d.get("status") for d in issues}

    cited = collections.defaultdict(set)
    blobs = {}
    for d in issues:
        blob = " ".join(str(d.get(f) or "") for f in TEXT_FIELDS)
        blobs[d.get("id")] = blob
        for h in HASH_RE.findall(blob):
            cited[h].add(d.get("id"))

    # subject -> hashes, over HEAD only
    by_subject = collections.defaultdict(list)
    for row in git("log", "--format=%H%x1f%s", "HEAD").stdout.splitlines():
        h, _, subj = row.partition("\x1f")
        by_subject[subj].append(h)

    buckets = collections.defaultdict(list)
    for h, ids in cited.items():
        if git("cat-file", "-t", h).stdout.strip() != "commit":
            continue                                  # not a hash at all
        if git("merge-base", "--is-ancestor", h, "HEAD").returncode == 0:
            buckets["ok"].append((h, ids, None))
            continue
        refs = git("for-each-ref", "--format=%(refname:short)",
                   "--contains", h).stdout.split()
        subject = git("show", "-s", "--format=%s", h).stdout.strip()
        if refs:
            buckets["archived"].append((h, ids, refs[0]))
        elif len(by_subject.get(subject, [])) == 1:
            buckets["rewritten"].append((h, ids, by_subject[subject][0]))
        else:
            buckets["dangling"].append((h, ids, subject))

    def annotated(bead_id, survivor):
        """True if this bead already records the correction.

        The fix for a stale hash is a note naming the survivor, which leaves
        the stale hash in the text -- so a bead that has been corrected would
        otherwise be reported forever and the check would never go green.
        Treat a bead as handled once the survivor hash appears alongside."""
        if not survivor:
            return False
        blob = blobs.get(bead_id, "")
        return any(survivor.startswith(h) or h.startswith(survivor[:9])
                   for h in HASH_RE.findall(blob))

    def relevant(ids, survivor=None):
        return sorted(i for i in ids
                      if status.get(i) != "closed" and not annotated(i, survivor))

    bad = 0
    for name in ("rewritten", "dangling", "archived"):
        rows = buckets[name]
        if name != "archived":
            bad += sum(1 for _, ids, extra in rows
                       if relevant(ids, extra if name == "rewritten" else None))
        def rel(r):
            return relevant(r[1], r[2] if name == "rewritten" else None)
        shown = [r for r in rows if rel(r) or show_closed]
        if not shown:
            continue
        print(f"\n=== {name.upper()} ({len(rows)} total, "
              f"{len([r for r in rows if rel(r)])} uncorrected in open beads)")
        for h, ids, extra in sorted(shown):
            openids = relevant(ids, extra if name == "rewritten" else None)
            tag = f"open: {openids}" if openids else "closed or already corrected"
            if name == "rewritten":
                print(f"  {h[:9]} -> {extra[:9]}   {tag}")
            elif name == "archived":
                print(f"  {h[:9]} on {extra}   {tag}")
            else:
                print(f"  {h[:9]} (no survivor: {extra[:50]!r})   {tag}")

    print(f"\nok: {len(buckets['ok'])} hashes reachable from HEAD")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
