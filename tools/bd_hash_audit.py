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

Usage:  tools/bd_hash_audit.py [--all] [--fix]
        --all also lists hashes cited only by closed beads (noise, mostly)
        --fix appends the corrections for the `rewritten' bucket to the open
              beads that cite them.  Without it, --fix's work is printed and
              nothing is written, so the plain run stays read-only

Exits 1 if any open bead cites a rewritten or dangling hash that has not
been acknowledged.  Acknowledge by naming the survivor in the bead, or by
writing 'HASH CORRECTION' / 'HASH CHECKED' into it when there is nothing
to point at (dangling) or nothing to fix (a deliberate archive ref).

--fix only ever touches the `rewritten' bucket, and only open beads.  That
bucket is the mechanical one -- the object exists and exactly one commit in
HEAD carries its subject, so there is a single right answer and no judgement
to exercise.  `dangling' has no survivor to name and `archived' is usually
deliberate; both need a human to decide what the bead should say, so --fix
leaves them alone and they keep failing the check until someone does.

Closed beads are left alone too, deliberately: the correction is mechanical
for them as well, but rewriting closed history in bulk is churn with no
reader.  uhexen2-zq7w.

Appends via `bd update --append-notes'.  NOT --notes, which replaces the
field outright and would destroy the bead's existing notes.
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

# A bead carrying one of these is treated as already reviewed, whatever
# state its hashes are in.  Needed because a dangling hash has no survivor
# to name and a deliberate archive reference has nothing to fix -- without
# an opt-out both would be reported forever.  Write one into the bead when
# you have looked and decided.
ACK_MARKERS = ("HASH CORRECTION", "HASH CHECKED")

# Bounds on the dangling-commit search in suggest_squashed_into().  Both are
# just there to keep the walk cheap: the distinctive files are the big ones,
# and a squash survivor is near its original in history, never 200 commits away.
PROBE_FILES = 5
PROBE_COMMITS = 80

# 7-40 hex chars.  Pure digits are excluded because the tracker is full of
# CRCs (17499, 38488, ...) that would otherwise be tested as abbreviated
# hashes and, at 5 digits, occasionally resolve.
HASH_RE = re.compile(r"\b(?=[0-9a-f]*[a-f])[0-9a-f]{7,40}\b")


def git(*args):
    return subprocess.run(("git",) + args, capture_output=True, text=True)


def load_issues():
    """Read the tracker, live if bd is on PATH.

    Not from .beads/issues.jsonl by preference: that file is written by the
    pre-commit export, so between a `bd update' and the next commit it is
    stale -- and "run the audit at session close, before committing" is
    exactly when this is used.  Reading the checked-in file there would have
    the check pass on corrections it cannot see, or fail on ones already
    made.  `bd export' writes the same records to stdout and touches nothing.
    """
    text = None
    try:
        r = subprocess.run(("bd", "export"), capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            text = r.stdout
    except OSError:
        pass                                          # no bd on PATH

    if text is None:
        with open(JSONL) as fh:
            text = fh.read()

    issues = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        if d.get("_type") == "memory":
            continue                                  # bd remember rows
        issues.append(d)
    return issues


def changed_files(h):
    """path -> lines touched, for one commit.  Beads exports excluded: every
    commit carries one and it would match everything."""
    files = {}
    for row in git("show", "--numstat", "--format=", "-M", h).stdout.splitlines():
        parts = row.split("\t")
        if len(parts) != 3:
            continue
        added, deleted, path = parts
        if added == "-":
            continue                                  # binary
        if "issues.jsonl" in path:
            continue
        files[path] = int(added) + int(deleted)
    return files


def suggest_squashed_into(h):
    """Best guess at the HEAD commit a dangling commit was squashed into.

    Subject matching is what bucketed it as dangling in the first place, and
    it cannot help: a squash keeps the subject of whichever commit came
    first, which is routinely unrelated work.  Really -- the .md5anim parser
    lives inside a commit about BC7 texture compression, and an entsearch
    config fix inside one about a server datagram guard.

    The diff survives a squash even when the subject does not, so match on
    that instead.  For each of the files the dead commit changed most, find
    the HEAD commits that changed that same file by exactly the same number
    of lines; a file with exactly one such commit casts a vote for it.  One
    file agreeing can be coincidence -- a doc rewritten twice by the same
    number of lines -- so several files agreeing is what makes it worth
    printing, and a tie prints nothing.

    Reported as evidence for a human, never applied by --fix.

    Returns (commit, path, churn, votes) or None.
    """
    files = changed_files(h)
    if not files:
        return None

    votes = collections.Counter()
    evidence = {}
    for path, churn in sorted(files.items(), key=lambda kv: -kv[1])[:PROBE_FILES]:
        hits = []
        for c in git("log", "--format=%H", "-n", str(PROBE_COMMITS),
                     "HEAD", "--", path).stdout.split():
            if changed_files(c).get(path) == churn:
                hits.append(c)
                if len(hits) > 1:
                    break                             # ambiguous for this file
        if len(hits) == 1:
            votes[hits[0]] += 1
            evidence.setdefault(hits[0], (path, churn))

    if not votes:
        return None
    ranked = votes.most_common(2)
    if len(ranked) > 1 and ranked[1][1] == ranked[0][1]:
        return None                                   # no clear winner
    best, count = ranked[0]
    path, churn = evidence[best]
    return (best, path, churn, count)


def bd_append_note(bead_id, text):
    """Append to a bead's notes.  Returns True on success.

    --append-notes rather than --notes: the latter replaces the field, which
    on a bead carrying a long investigation is silent data loss.
    """
    r = subprocess.run(("bd", "update", bead_id, "--append-notes", text),
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(f"  bd update {bead_id} failed: "
                         f"{(r.stderr or r.stdout).strip()}\n")
        return False
    return True


def apply_fixes(pending, write):
    """Record `old -> new' corrections in the open beads that cite them.

    pending maps bead id -> list of (old, new, subject).  One append per bead
    however many hashes it cites, so a bead that named three rewritten commits
    does not collect three separate notes.
    """
    if not pending:
        return 0

    print(f"\n=== {'APPLYING' if write else 'WOULD APPLY'} "
          f"{sum(len(v) for v in pending.values())} correction(s) "
          f"to {len(pending)} open bead(s)")

    done = 0
    for bead_id in sorted(pending):
        lines = ["HASH CORRECTION (tools/bd_hash_audit.py): the commit(s) cited",
                 "above were rewritten by a rebase.  Survivors, matched on subject:",
                 ""]
        for old, new, subject in sorted(pending[bead_id]):
            lines.append(f"    {old[:9]} -> {new[:9]}  {subject}")
        note = "\n".join(lines)
        print(f"  {bead_id}: {len(pending[bead_id])} hash(es)")
        if not write or bd_append_note(bead_id, note):
            done += 1

    if not write:
        print("\n  (dry run -- pass --fix to write these)")
    return done


def main():
    show_closed = "--all" in sys.argv
    do_fix = "--fix" in sys.argv
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
        the stale hash in the text -- so a corrected bead would otherwise be
        reported forever and the check could never go green.  Two ways to be
        handled:

          - the survivor hash appears in the bead as well (rewritten case), or
          - the bead carries one of ACK_MARKERS, which is the only option for
            a dangling hash (no survivor to name) or for an archive reference
            that is deliberate.
        """
        blob = blobs.get(bead_id, "")
        if any(m in blob for m in ACK_MARKERS):
            return True
        if not survivor:
            return False
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
                if openids:
                    guess = suggest_squashed_into(h)
                    if guess:
                        c, path, churn, votes = guess
                        print(f"      likely squashed into {c[:9]} "
                              f"({votes} file(s) agree; {path}, "
                              f"{churn} lines in both)")

    pending = collections.defaultdict(list)
    for h, ids, survivor in buckets["rewritten"]:
        subject = git("show", "-s", "--format=%s", survivor).stdout.strip()
        for bead_id in relevant(ids, survivor):
            pending[bead_id].append((h, survivor, subject))

    if pending and (do_fix or bad):
        applied = apply_fixes(pending, do_fix)
        if do_fix:
            # Everything just annotated now counts as acknowledged, so only
            # the buckets --fix deliberately does not touch should still fail.
            bad -= applied

    print(f"\nok: {len(buckets['ok'])} hashes reachable from HEAD")
    return 1 if bad > 0 else 0


if __name__ == "__main__":
    sys.exit(main())
