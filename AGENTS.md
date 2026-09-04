# Agent Instructions

This project has no in-tree issue tracker.  The **bd (beads)** tracker it used to
carry is retired; `(uhexen2-xxxx)` ids on older commits are historical breadcrumbs,
not live issues.  Do not reintroduce `.beads/` to the tree.

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
  overwritten by merge`). Any tool that stages on its own schedule — and any
  other session touching the index — makes this fire unpredictably.
- Stashing to clear that is itself a race, if something can re-stage between
  the `git stash` and the `git merge`.
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

## Landing the Plane (Session Completion)

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
