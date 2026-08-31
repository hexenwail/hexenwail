# Agent Instructions

This project uses **bd** (beads) for issue tracking. Run `bd onboard` to get started.

## Work in a worktree

More than one agent session runs against this checkout at a time. A git checkout
has **one index, one HEAD and one `MERGE_HEAD`** — so concurrent sessions do not
merely race on files, they can commit each other's work.

That is not hypothetical. On 2026-08-31 one session was mid-merge of
`sezero/master` (conflicts resolved and staged, not yet committed) when another
session ran `git commit` for an unrelated documentation pass. Git had no way to
tell the two apart. The result is `914114182`: a **merge commit** whose second
parent is upstream `46c12d854`, carrying the resolved `snd_timidity.c` and the
entire `libs/timidity` sync, under the message
`docs(parity): structural audit against Ironwail v0.8.2`. Right content, wrong
commit — and not separable afterwards without rewriting history under a session
that is still working in it.

Sharp edges in the same shared checkout:

- `git merge` **refuses to start** while anything is staged, even a path the
  merge does not touch (`Your local changes to the following files would be
  overwritten by merge`). bd re-stages `.beads/issues.jsonl` after every write,
  on its own schedule, so this fires unpredictably.
- Stashing to clear that is itself a race — bd can re-export and re-stage
  between the `git stash` and the `git merge`.
- `git stash`, `git rebase`, `git bisect` and `git checkout` are all
  checkout-wide. Another session's working tree changes underneath it with no
  warning.

So: **do the work in a worktree.**

```bash
git worktree add .claude/worktrees/<slug> -b worktree-<slug>
cd .claude/worktrees/<slug>
# ... edit, build, commit ...
git worktree remove .claude/worktrees/<slug>   # once the branch has landed
```

`.claude/worktrees/<slug>` on a `worktree-<slug>` branch is the convention
already in use here.

**Beads is the exception, and it does not move.** `.gitignore` tracks only
`.beads/issues.jsonl`; the Dolt database, `config.yaml` and the server lock
files are ignored, so a fresh worktree gets the JSONL and nothing that can read
it. Run `bd` from the main checkout — one database, one writer. Code work goes
to the worktree; issue work stays home.

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
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
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
<!-- END BEADS INTEGRATION -->
