# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd onboard` to get started.

## First-time setup (fresh clone)

The issue history is version-controlled in `.beads/issues.jsonl`; the local Dolt
database under `.beads/` is git-ignored, so a fresh clone has **no materialized
database** until you build one from the tracked JSONL. `bd onboard`/`bd init`
alone create an *empty* database — you must import the existing issues:

```bash
# Build the local database AND load all tracked issues in one step:
bd init --from-jsonl --non-interactive

# Already ran `bd init`? Just import into the existing database:
bd import            # reads .beads/issues.jsonl

bd ready             # verify: should list the open, unblocked issues
```

`bd` uses an embedded Dolt engine by default (no server required). After local
writes it re-exports to `.beads/issues.jsonl`; commit that file alongside code so
issue state stays in sync. Keep statuses to bd's valid set (`open`,
`in_progress`, `blocked`, `closed`) — non-standard values like `completed` or
`deferred` will fail to import for the next person.

## Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --status in_progress  # Claim work
bd close <id>         # Complete work
bd sync               # Sync with git
```

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd sync
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds

