# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Check cited commit hashes** - `python3 tools/bd_hash_audit.py`
   ```bash
   python3 tools/bd_hash_audit.py        # exits 1 if an open bead cites a dead hash
   python3 tools/bd_hash_audit.py --fix  # applies the mechanical corrections
   ```
   Quiet when everything is fine. It reads `bd export`, so run it before
   committing and it still sees what you just wrote. See below for why.
5. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
6. **Clean up** - Clear stashes, prune remote branches
7. **Verify** - All changes committed AND pushed
8. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push

## Citing commits in beads

**Write the commit SUBJECT, not just the hash.** A hash you record before its
beads commit is squashed into the code commit is not occasionally stale, it is
*guaranteed* stale — squashing replaces the commit and its hash with a new one.
That is the house workflow, so it happens every time; the audit found 76
rewritten and 23 dangling hashes accumulated this way (uhexen2-zq7w).

Subjects survive a rebase, hashes do not. So write:

> fixed in `feat(server): sv_netsort decides who survives a full datagram`
> (4724daf2c at time of writing)

and the reader can always find it with `git log --grep`, whatever the hash
became. Step 4 above is the backstop, not the plan.

Note the squash keeps the subject of whichever commit came *first*, which is
routinely unrelated work — the MD5 `.md5anim` parser lives inside a commit
titled after BC7 texture compression. `bd_hash_audit.py` matches those on the
diff and prints the survivor.

**`bd update --notes` REPLACES the notes field.** Use `--append-notes` to add to
it, or you will silently destroy an existing investigation. Same for
`--description` and `--design`. Every prior version is recoverable from
`git show <commit>:.beads/issues.jsonl` if you do.
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->


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
