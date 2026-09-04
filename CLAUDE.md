# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

## Issue tracking

There is no in-tree issue tracker.  This project used **bd (beads)** until it was
retired; you will still see `(uhexen2-xxxx)` ids on older commits and in the docs
under `history/`.  Those are historical references — read them as breadcrumbs into
the reasoning behind a change, not as live issues you can look up.  Do not add new
ones, and do not reintroduce `.beads/` to the tree.

Track work for the current session however the session calls for it.

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **Run quality gates** (if code changed) - Tests, linters, builds
2. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   git push
   git status  # MUST show "up to date with origin"
   ```
3. **Clean up** - Clear stashes, prune remote branches
4. **Verify** - All changes committed AND pushed
5. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

## Citing commits

**Write the commit SUBJECT, not just the hash.** The house workflow squashes, and
a squash replaces the commit and its hash with a new one — so a hash recorded
against work that has not landed yet is not occasionally stale, it is guaranteed
stale.  Subjects survive a rebase, hashes do not. So write:

> fixed in `feat(server): sv_netsort decides who survives a full datagram`
> (4724daf2c at time of writing)

and the reader can always find it with `git log --grep`, whatever the hash became.

Note the squash keeps the subject of whichever commit came *first*, which is
routinely unrelated work — the MD5 `.md5anim` parser lives inside a commit titled
after BC7 texture compression.  When a subject looks wrong for the change it
claims, search the diff, not the log.


## Reading Ironwail

The parity work reads upstream constantly.  **Read it by ref, never by path.**

    git -C ../ironwail show origin/master:Quake/gl_screen.c
    git -C ../ironwail grep <sym> origin/master -- Quake

The checkout at `../ironwail` has two remotes.  `origin` is pristine Ironwail;
its checked-out HEAD is `bobberb/ironwail`, a **Hexen II fork** ~190 commits
and 18k lines ahead of upstream, with a dirty working tree on top.  So a read
of the *worktree* can hand you the fork's Hexen II adaptation and look like
upstream — and it will look *more* plausible than the real thing precisely
because it already speaks Hexen II.

Caught live: reading `SV_PrintMapChecklist` out of the worktree returns a
`hexen2_mode ? "soundtype" : "sounds"` music-track check.  That is the fork's
guess, and it is also wrong for this engine — Hexen II's music comes from the
worldspawn `CD` / `MIDI` keys, which `ED_ParseEdict` intercepts by name, and
`soundtype` is the field that picks a door's sound set.  Porting from that read
would have shipped someone else's mistake under Ironwail's name.

The fork is still worth reading as **prior art** — someone else's answer to the
same adaptation question.  Cite it as "bobberb/ironwail fork", never as
"Ironwail", and treat what you take as a design borrowed rather than parity
achieved.  uhexen2-a5nn.39

## Build & Test

_Add your build and test commands here_

```bash
# Example:
# npm install
# npm test
```

## Architecture Overview

_Add a brief overview of your project architecture_

## Conventions & Patterns

_Add your project-specific conventions here_
